#pragma once
#ifndef WEBSOCKET_CHANNEL_HPP
#define WEBSOCKET_CHANNEL_HPP

#include <memory>
#include <stdexcept>
#include <utility>
#include <WebSocketConnection.hpp>

class Process;

class WebSocketChannel {
private:
    std::shared_ptr<WebSocketConnection> connection;

public:
    explicit WebSocketChannel(std::shared_ptr<WebSocketConnection> connection)
        : connection(std::move(connection)) {
        if (!this->connection) {
            throw std::invalid_argument("WebSocketChannel requires a connection");
        }
    }

    void send(const GraphFrame& frame) { connection->sendFrame(frame); }
    bool receive(Process& process) { return connection->poll(process); }
};

#endif
