#include "net/outbound_queue.h"

#include <stdexcept>

namespace rtsp::net {

OutboundQueue::OutboundQueue(std::size_t max_bytes, std::size_t max_messages)
    : max_bytes_(max_bytes), max_messages_(max_messages) {
    if (max_bytes_ == 0) {
        throw std::invalid_argument("outbound queue byte limit must be positive");
    }
}

bool OutboundQueue::push(OutboundMessage message) {
    if (message.wire.size() > max_bytes_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || bytes_ > max_bytes_ - message.wire.size()
        || (max_messages_ != 0 && messages_.size() >= max_messages_)) {
        return false;
    }
    bytes_ += message.wire.size();
    messages_.push_back(std::move(message));
    condition_.notify_one();
    return true;
}

bool OutboundQueue::pop(OutboundMessage& message) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return closed_ || !messages_.empty(); });
    if (messages_.empty()) {
        return false;
    }
    message = std::move(messages_.front());
    messages_.pop_front();
    bytes_ -= message.wire.size();
    return true;
}

void OutboundQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    condition_.notify_all();
}

bool OutboundQueue::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t OutboundQueue::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

}  // namespace rtsp::net
