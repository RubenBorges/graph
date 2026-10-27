#pragma once
#ifndef WIRE_HPP
#define WIRE_HPP

#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <limits>
#include <system_error>
#include <vector>
#include <cstring>
#include <sys/socket.h> // Or <winsock2.h> on Windows
#include <unistd.h>

using WaveID = uint32_t;
using ID = uint32_t;
using Message = std::vector<uint8_t>;

enum class FrameKind : uint8_t {
    Data,
    Acknowledgement,
    Cleanup
};

// Enforce strict binary layouts across the wire
#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;       // Fixed sync marker: "GRPH" (0x47525048)
    uint32_t waveId;     // Tracking ID for concurrent graph executions
    uint8_t kind;
    uint32_t payloadLength; // Exact byte count of the data vector
};
#pragma pack(pop)

using CRC32_t = uint32_t;

struct GraphFrame{
    WaveID waveID;
    Message payload;
    FrameKind kind{FrameKind::Data};
};

class Wire {
private:
    int socket_fd;
    std::vector<uint8_t> readBuffer;
    const uint32_t MAGIC_MARKER = 0x47525048; // "GRPH"

    // Internal CRC32 checksum calculator
    uint32_t compute_crc32(const uint8_t* data, size_t length) const {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
                else crc >>= 1;
            }
        }
        return ~crc;
    }

public:
    // Tie the Wire instance to a physical open socket descriptor
    explicit Wire(int fd) ;
    
    ~Wire();
    // Move-only semantics to prevent multiple objects owning the same socket descriptor
    Wire(const Wire&) = delete;
    Wire& operator=(const Wire&) = delete;
    Wire(Wire&& other) noexcept ;
    
    Wire& operator=(Wire&& other) noexcept ;
    // OUTBOUND: Formats and pushes data directly into the socket connection
    bool sendFrame(const GraphFrame& frame) const ;

    // INBOUND: Polls data from network buffers and feeds complete frames into your Node/Process
    template<typename ProcessNodeType>
    bool receiveAndParse(ProcessNodeType& processNode) {
        if (socket_fd < 0) return false;

        uint8_t tempStackBuffer[1024];
        ssize_t bytesRead = recv(socket_fd, tempStackBuffer, sizeof(tempStackBuffer), MSG_DONTWAIT);
        
        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return false;
            throw std::system_error(errno, std::generic_category(), "recv");
        }
        if (bytesRead == 0) {
            throw std::runtime_error("Peer closed the socket connection");
        }

        readBuffer.insert(readBuffer.end(), tempStackBuffer, tempStackBuffer + bytesRead);
        bool receivedFrame = false;

        while (true) {
            if (readBuffer.size() < sizeof(MessageHeader)) {
                break; // Need more data for header configuration
            }

            MessageHeader header;
            std::memcpy(&header, readBuffer.data(), sizeof(MessageHeader));

            // Synchronization Guard
            if (header.magic != MAGIC_MARKER) {
                std::cerr << "[Wire Exception] Stream out of sync! Invalid magic marker. Dropping link.\n";
                readBuffer.clear();
                close(socket_fd);
                socket_fd = -1;
                return receivedFrame;
            }

            size_t totalFrameSize = sizeof(MessageHeader) + header.payloadLength + sizeof(CRC32_t);

            if (readBuffer.size() < totalFrameSize) {
                break; // Full message frame body has not fully arrived over the wire yet
            }

            // Extract variable length payload
            const uint8_t* payload_start = readBuffer.data() + sizeof(MessageHeader);
            std::vector<uint8_t> payload(payload_start, payload_start + header.payloadLength);

            // Extract CRC checksum trailer
            CRC32_t received_crc;
            std::memcpy(&received_crc, payload_start + header.payloadLength, sizeof(CRC32_t));

            // Verification Check
            if (compute_crc32(readBuffer.data(), sizeof(MessageHeader) + header.payloadLength) != received_crc) {
                std::cerr << "[Wire Error] Checksum verification mismatch! Frame discarded.\n";
            } else {
                // Instantiation and transmission to target execution topology loop
                if (header.kind > static_cast<uint8_t>(FrameKind::Cleanup)) {
                    std::cerr << "[Wire Error] Invalid frame kind. Frame discarded.\n";
                    readBuffer.erase(readBuffer.begin(), readBuffer.begin() + totalFrameSize);
                    continue;
                }
                GraphFrame frame{
                    header.waveId,
                    std::move(payload),
                    static_cast<FrameKind>(header.kind)
                };
                processNode.receive(frame);
                receivedFrame = true;
            }

            // Consume frame from state tracker buffer array
            readBuffer.erase(readBuffer.begin(), readBuffer.begin() + totalFrameSize);
        }
        return receivedFrame;
    }
};

#endif // WIRE_HPPs