#include <Wire.hpp>
#include <cerrno>
#include <limits>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

// FIX: Dropped invalid out-of-line 'explicit' keyword specifier
Wire::Wire(int fd) : socket_fd(fd) {}
    
Wire::~Wire() {
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

Wire::Wire(Wire&& other) noexcept : socket_fd(other.socket_fd), readBuffer(std::move(other.readBuffer)) {
    other.socket_fd = -1;
}

Wire& Wire::operator=(Wire&& other) noexcept {
    if (this != &other) {
        if (socket_fd >= 0) close(socket_fd);
        socket_fd = other.socket_fd;
        readBuffer = std::move(other.readBuffer); // FIX: Named 'readBuffer' uniformly
        other.socket_fd = -1;
    }
    return *this;
}

bool Wire::sendFrame(const GraphFrame& frame) const {
    if (socket_fd < 0) return false;
    if (frame.payload.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("Frame payload is too large to encode");
    }
    if (frame.kind > FrameKind::Cleanup) {
        throw std::invalid_argument("Invalid frame kind");
    }

    MessageHeader header;
    header.magic = MAGIC_MARKER;
    header.waveId = static_cast<uint32_t>(frame.waveID); // FIX: Pointing to frame.waveID matching definition layout
    header.kind = static_cast<uint8_t>(frame.kind);
    header.payloadLength = static_cast<uint32_t>(frame.payload.size());

    std::vector<uint8_t> wirePacket;
    wirePacket.reserve(sizeof(MessageHeader) + frame.payload.size() + sizeof(CRC32_t));
    
    const uint8_t* headerPtr = reinterpret_cast<const uint8_t*>(&header);
    wirePacket.insert(wirePacket.end(), headerPtr, headerPtr + sizeof(MessageHeader));
    wirePacket.insert(wirePacket.end(), frame.payload.begin(), frame.payload.end());
    uint32_t crc = compute_crc32(wirePacket.data(), wirePacket.size());
    const uint8_t* crcPtr = reinterpret_cast<const uint8_t*>(&crc);
    wirePacket.insert(wirePacket.end(), crcPtr, crcPtr + sizeof(CRC32_t));

    size_t total_sent = 0;
    while (total_sent < wirePacket.size()) {
        ssize_t sent = send(
            socket_fd,
            wirePacket.data() + total_sent,
            wirePacket.size() - total_sent,
            0
        );
        if (sent < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "send");
        }
        if (sent == 0) return false;
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}
