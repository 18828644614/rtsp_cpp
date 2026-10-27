#include "net/outbound_queue.h"

#include <stdexcept>
#include <utility>

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
    if (closed_ || bytes_ > max_bytes_ - message.wire.size()
        || (max_messages_ != 0 && messages_.size() + (sending_ ? 1 : 0) >= max_messages_)) {
        return false;
    }
    bytes_ += message.wire.size();
    messages_.push_back(std::move(message));
    return true;
}

bool OutboundQueue::try_pop(OutboundMessage& message) {
    if (messages_.empty()) {
        return false;
    }
    message = std::move(messages_.front());
    messages_.pop_front();
    bytes_ -= message.wire.size();
    return true;
}

bool OutboundQueue::empty() const {
    return messages_.empty();
}

bool OutboundQueue::take_for_send(OutboundMessage& message) {
    if (sending_) {
        throw std::logic_error("complete current send before taking another message");
    }
    if (messages_.empty()) {
        return false;
    }
    message = std::move(messages_.front());
    messages_.pop_front();
    sending_bytes_ = message.wire.size();
    sending_ = true;
    return true;
}

void OutboundQueue::complete_send() {
    if (!sending_) {
        throw std::logic_error("no current send");
    }
    bytes_ -= sending_bytes_;
    sending_bytes_ = 0;
    sending_ = false;
}

void OutboundQueue::close() {
    closed_ = true;
}

bool OutboundQueue::closed() const {
    return closed_;
}

std::size_t OutboundQueue::bytes() const {
    return bytes_;
}

}  // namespace rtsp::net
