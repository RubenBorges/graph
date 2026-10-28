#pragma once
#ifndef PROCESS_HPP
#define PROCESS_HPP

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <variant>
#include <compare>
#include <print>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <Wire.hpp>
#include <WebSocketChannel.hpp>

class Process;

struct Edge {
    ID source;
    ID destination;
    auto operator<=>(const Edge&) const = default;
};

template<typename ProcessNode>
class PointerChannel {
private:
    ProcessNode* target_node;
public:
    explicit PointerChannel(ProcessNode* node) : target_node(node) {
        if (!target_node) throw std::invalid_argument("PointerChannel target cannot be null");
    }
    void send(const GraphFrame& frame) { target_node->receive(frame); }
};

class SocketChannel {
private:
    std::shared_ptr<Wire> wire;
public:
    explicit SocketChannel(std::shared_ptr<Wire> connection) : wire(std::move(connection)) {
        if (!wire) throw std::invalid_argument("SocketChannel requires a Wire connection");
    }
    void send(const GraphFrame& frame) {
        if (!wire->sendFrame(frame)) {
            throw std::runtime_error("Failed to send frame on socket channel");
        }
    }
    bool receive(Process& process) { return wire->receiveAndParse(process); }
};

using Channel = std::variant<PointerChannel<Process>, SocketChannel, WebSocketChannel>;

class Process {
public:
    ID identity;
    
private:
    std::optional<ID> parent;
    std::vector<ID> children;
    std::vector<ID> knownProcesses;
    std::vector<Edge> knownChannels;
    std::unordered_map<ID, Channel> activeChannels;
    std::unordered_map<WaveID, size_t> expectedMessagesByWave;
    std::set<WaveID> activeWaves;
    std::set<WaveID> completedWaves;
    std::set<WaveID> cleanedWaves;

public:
    Process() = delete;
    explicit Process(ID id);
    ~Process() = default;   

    void start(GraphFrame frame);
    void pollChannels();
    bool isWaveCompleted(WaveID waveID) const;
    bool isWaveCleaned(WaveID waveID) const;
    bool isProcessKnown(ID id) const;
    void position(ID id, std::vector<Edge> neighbours);
    void receive(GraphFrame frame);
    void registerChannel(ID neighbourID, Channel channelImplementation);
    void addChild(ID childID);
    void addChild(Process* child);
    void setParent(ID parentID);
    uint8_t getActiveWaves(){ return activeWaves.size();}
    uint8_t getCompletedWaves(){ return completedWaves.size();}

private:
    void go(GraphFrame frame);
    void back(GraphFrame frame);
    void completeWave(WaveID waveID);
};
#endif 
