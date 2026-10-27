#include <CentralCoordinatorIDGen.hpp>
#include <iostream>
#include <cstdint>
#include <stdexcept>

    // Initialize with a unique ID for this specific machine/node (0-15)
    CentralCoordinatorIDGen::CentralCoordinatorIDGen(uint8_t server_id) {
        if (server_id > SERVER_MASK) {
            throw std::invalid_argument("Server ID must be between 0 and 15 (4 bits max).");
        }
        // Shift the server ID to the highest 4 bits
        // e.g., 0000'0000'0000'1011 (11) becomes 1011'0000'0000'0000
        this->server_shift_value = static_cast<uint16_t>(server_id) << 12;
    }

    // Thread-safe ID generation
    uint16_t CentralCoordinatorIDGen::generate_id() {
        // Atomically increment and get the sequence
        uint16_t current_seq = sequence.fetch_add(1, std::memory_order_relaxed);

        // Mask the sequence to 12 bits so it cleanly wraps back to 0 at 4096
        current_seq &= SEQUENCE_MASK;

        // Combine the shifted Server ID and the Local Sequence using bitwise OR
        return server_shift_value | current_seq;
    };
