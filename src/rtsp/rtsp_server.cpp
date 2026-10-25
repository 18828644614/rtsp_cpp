#include "rtsp/rtsp_server.h"

#include "rtsp/rtsp_server_connection.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {

RtspServer::RtspServer(std::string address, std::uint16_t port, std::string input_path)
    : address_(std::move(address)), port_(port), input_path_(std::move(input_path)) {}

RtspServer::~RtspServer() {
    stop();
    stop_connections();
    listener_.close();
}

void RtspServer::run() {
    if (listener_.is_open()) {
        throw std::logic_error("server is already running");
    }
    stopping_.store(false, std::memory_order_release);
    listener_.listen(address_, port_);
    std::cout << "RTSP server listening on " << address_ << ':'
              << listener_.local_port() << std::endl;

    try {
        while (!stopping_.load(std::memory_order_acquire)) {
            // 删除已经断开的连接
            reap_finished_connections();
            if (!listener_.wait_readable(100)) {
                continue;
            }

            auto connection = std::make_unique<RtspServerConnection>(input_path_);
            connection->accept_from(listener_);
            std::cout << "Client connected, Session=" << connection->session_id() << std::endl;
            connection->start();
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_.push_back(std::move(connection));
            }
        }
    } catch (const std::exception&) {
        if (!stopping_.load(std::memory_order_acquire)) {
            stop_connections();
            listener_.close();
            throw;
        }
    }

    listener_.close();
    stop_connections();
}

void RtspServer::stop() {
    stopping_.store(true, std::memory_order_release);
    listener_.close();
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        connections_[index]->stop();
    }
}

std::uint16_t RtspServer::local_port() const {
    return listener_.local_port();
}

std::size_t RtspServer::connection_count() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}

void RtspServer::reap_finished_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::size_t index = 0;
    while (index < connections_.size()) {
        if (!connections_[index]->finished()) {
            ++index;
            continue;
        }
        connections_[index]->join();
        connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void RtspServer::stop_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        connections_[index]->stop();
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        connections_[index]->join();
    }
    connections_.clear();
}

}  // namespace rtsp
