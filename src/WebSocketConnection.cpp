#include <WebSocketConnection.hpp>

#include <Process.hpp>
#include <Wire.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using WebSocketStream = websocket::stream<beast::ssl_stream<tcp::socket>>;

constexpr uint32_t MAGIC_MARKER = 0x47525048;
constexpr size_t MAX_FRAME_SIZE = 16 * 1024 * 1024;
constexpr size_t MAX_PAYLOAD_SIZE =
    MAX_FRAME_SIZE - sizeof(MessageHeader) - sizeof(CRC32_t);

uint32_t computeCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
        }
    }
    return ~crc;
}

std::vector<uint8_t> encodeFrame(const GraphFrame& frame) {
    if (frame.payload.size() > MAX_PAYLOAD_SIZE ||
        frame.payload.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("WebSocket graph frame payload is too large");
    }
    if (frame.kind > FrameKind::Position) {
        throw std::invalid_argument("Invalid graph frame kind");
    }

    MessageHeader header{};
    header.magic = htonl(MAGIC_MARKER);
    header.waveId = htonl(frame.waveID);
    header.kind = static_cast<uint8_t>(frame.kind);
    header.payloadLength = htonl(static_cast<uint32_t>(frame.payload.size()));

    std::vector<uint8_t> bytes(sizeof(header) + frame.payload.size() + sizeof(CRC32_t));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::copy(frame.payload.begin(), frame.payload.end(), bytes.begin() + sizeof(header));

    const uint32_t checksum = htonl(computeCRC32(bytes.data(), sizeof(header) + frame.payload.size()));
    std::memcpy(bytes.data() + sizeof(header) + frame.payload.size(), &checksum, sizeof(checksum));
    return bytes;
}

GraphFrame decodeFrame(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < sizeof(MessageHeader) + sizeof(CRC32_t) || bytes.size() > MAX_FRAME_SIZE) {
        throw std::runtime_error("Malformed WebSocket graph frame size");
    }

    MessageHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const uint32_t payloadLength = ntohl(header.payloadLength);
    if (ntohl(header.magic) != MAGIC_MARKER ||
        header.kind > static_cast<uint8_t>(FrameKind::Position) ||
        bytes.size() != sizeof(header) + static_cast<size_t>(payloadLength) + sizeof(CRC32_t)) {
        throw std::runtime_error("Malformed WebSocket graph frame header");
    }

    uint32_t checksum;
    std::memcpy(&checksum, bytes.data() + sizeof(header) + payloadLength, sizeof(checksum));
    if (ntohl(checksum) != computeCRC32(bytes.data(), sizeof(header) + payloadLength)) {
        throw std::runtime_error("WebSocket graph frame checksum mismatch");
    }

    const auto payloadBegin = bytes.begin() + sizeof(header);
    const auto payloadEnd = payloadBegin + payloadLength;
    return GraphFrame{
        ntohl(header.waveId),
        Message(payloadBegin, payloadEnd),
        static_cast<FrameKind>(header.kind)
    };
}

} // namespace

struct WebSocketConnection::Impl {
    explicit Impl(
        std::unique_ptr<WebSocketStream> websocket,
        TLSContext tlsContext,
        std::shared_ptr<asio::io_context> ioContext
    ) : tlsContext(std::move(tlsContext)),
        ioContext(std::move(ioContext)),
        stream(std::move(websocket)) {}

    ~Impl() {
        stopping.store(true);
        boost::system::error_code ignored;
        stream->next_layer().next_layer().shutdown(tcp::socket::shutdown_both, ignored);
        stream->next_layer().next_layer().close(ignored);
        if (reader.joinable()) {
            reader.join();
        }
    }

    void start() {
        reader = std::thread([this] { readLoop(); });
    }

    void readLoop() noexcept {
        try {
            beast::flat_buffer buffer;
            while (!stopping.load()) {
                stream->read(buffer);
                if (!stream->got_binary()) {
                    throw std::runtime_error("WebSocket graph connections require binary frames");
                }

                std::vector<uint8_t> bytes(buffer.size());
                asio::buffer_copy(asio::buffer(bytes), buffer.data());
                buffer.consume(buffer.size());

                GraphFrame frame = decodeFrame(bytes);
                {
                    std::lock_guard lock(queueMutex);
                    receivedFrames.push_back(std::move(frame));
                }
            }
        } catch (...) {
            if (!stopping.load()) {
                std::lock_guard lock(queueMutex);
                failure = std::current_exception();
            }
        }
    }

    void rethrowFailure() {
        std::exception_ptr currentFailure;
        {
            std::lock_guard lock(queueMutex);
            currentFailure = failure;
        }
        if (currentFailure) {
            std::rethrow_exception(currentFailure);
        }
    }

    TLSContext tlsContext;
    std::shared_ptr<asio::io_context> ioContext;
    std::unique_ptr<WebSocketStream> stream;
    std::thread reader;
    std::atomic<bool> stopping{false};
    std::mutex writeMutex;
    std::mutex queueMutex;
    std::deque<GraphFrame> receivedFrames;
    std::exception_ptr failure;
};

WebSocketConnection::WebSocketConnection(std::unique_ptr<Impl> implementation)
    : implementation(std::move(implementation)) {
    this->implementation->start();
}

WebSocketConnection::~WebSocketConnection() = default;

std::shared_ptr<WebSocketConnection> WebSocketConnection::connect(
    const std::string& host,
    const std::string& service,
    const std::string& target,
    TLSContext tlsContext
) {
    if (!tlsContext) {
        throw std::invalid_argument("WebSocket client requires a TLS context");
    }

    auto ioContext = std::make_shared<asio::io_context>();
    tcp::resolver resolver(*ioContext);
    const auto endpoints = resolver.resolve(host, service);
    tcp::socket socket(*ioContext);
    asio::connect(socket, endpoints);

    auto stream = std::make_unique<WebSocketStream>(std::move(socket), *tlsContext);
    stream->read_message_max(MAX_FRAME_SIZE);
    stream->next_layer().set_verify_mode(asio::ssl::verify_peer);
    stream->next_layer().set_verify_callback(asio::ssl::host_name_verification(host));
    if (SSL_set_tlsext_host_name(stream->next_layer().native_handle(), host.c_str()) != 1) {
        throw std::runtime_error("Failed to configure TLS server name: " +
                                 std::to_string(ERR_get_error()));
    }
    stream->next_layer().handshake(asio::ssl::stream_base::client);
    stream->handshake(host + ":" + service, target);

    return std::shared_ptr<WebSocketConnection>(
        new WebSocketConnection(std::make_unique<Impl>(
            std::move(stream),
            std::move(tlsContext),
            std::move(ioContext)
        ))
    );
}

std::shared_ptr<WebSocketConnection> WebSocketConnection::accept(
    std::shared_ptr<asio::io_context> ioContext,
    tcp::socket socket,
    TLSContext tlsContext,
    const std::string& target
) {
    if (!ioContext || &socket.get_executor().context() != ioContext.get()) {
        throw std::invalid_argument("Accepted socket must use the supplied io_context");
    }
    if (!tlsContext) {
        throw std::invalid_argument("WebSocket server requires a TLS context");
    }

    auto stream = std::make_unique<WebSocketStream>(std::move(socket), *tlsContext);
    stream->read_message_max(MAX_FRAME_SIZE);
    stream->next_layer().set_verify_mode(
        asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert
    );
    stream->next_layer().handshake(asio::ssl::stream_base::server);

    X509* peerCertificate = SSL_get1_peer_certificate(stream->next_layer().native_handle());
    if (!peerCertificate) {
        throw std::runtime_error("WebSocket peer did not present a client certificate");
    }
    X509_free(peerCertificate);
    if (SSL_get_verify_result(stream->next_layer().native_handle()) != X509_V_OK) {
        throw std::runtime_error("WebSocket client certificate verification failed");
    }

    beast::flat_buffer requestBuffer;
    beast::http::request_parser<beast::http::empty_body> requestParser;
    requestParser.header_limit(16 * 1024);
    requestParser.body_limit(0);
    beast::http::read(stream->next_layer(), requestBuffer, requestParser);
    const auto request = requestParser.release();
    if (request.target() != target) {
        throw std::runtime_error("WebSocket request target does not match the configured route");
    }
    stream->accept(request);
    return std::shared_ptr<WebSocketConnection>(
        new WebSocketConnection(std::make_unique<Impl>(
            std::move(stream),
            std::move(tlsContext),
            std::move(ioContext)
        ))
    );
}

void WebSocketConnection::sendFrame(const GraphFrame& frame) {
    implementation->rethrowFailure();
    const auto bytes = encodeFrame(frame);
    std::lock_guard lock(implementation->writeMutex);
    implementation->rethrowFailure();
    implementation->stream->binary(true);
    implementation->stream->write(asio::buffer(bytes));
}

bool WebSocketConnection::poll(Process& process) {
    std::deque<GraphFrame> frames;
    {
        std::lock_guard lock(implementation->queueMutex);
        frames.swap(implementation->receivedFrames);
    }

    for (auto& frame : frames) {
        process.receive(std::move(frame));
    }
    implementation->rethrowFailure();
    return !frames.empty();
}
