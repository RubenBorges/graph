#pragma once 
#ifndef CENTRALCOORDINATORIDGEN_HPP
#define CENTRALCOORDINATORIDGEN_HPP

#include <iostream>
#include <cstdint>
#include <atomic>
#include <stdexcept>

class CentralCoordinatorIDGen {
private:
    uint16_t server_shift_value;
    std::atomic<uint16_t> sequence{0};

    // Masks to isolate bits and prevent overflows
    static constexpr uint16_t SERVER_MASK = 0x0F;   // 4 bits (max 15)
    static constexpr uint16_t SEQUENCE_MASK = 0x0FFF; // 12 bits (max 4095)

public:
    // Initialize with a unique ID for this specific machine/node (0-15)
    explicit CentralCoordinatorIDGen(uint8_t server_id) ;
    
    // Thread-safe ID generation
    uint16_t generate_id(); 
};
#endif // CENTRALCOORDINATORIDGEN_HPP

