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
        || (max_messages_ != 0 && messages_.size() + (sending_ ? 1 : 0) >= max_messages_)) {
        return false;
    }
    bytes_ += message.wire.size();
    messages_.push_back(std::move(message));
    condition_.notify_one();
    return true;
}

// 未关闭	有	取出消息，返回 true
// 已关闭	有	继续取出剩余消息，返回 true
// 已关闭	无	没有消息可取，返回 false
bool OutboundQueue::pop(OutboundMessage& message) {
    std::unique_lock<std::mutex> lock(mutex_);
    // 队列被关闭 或者 消息队列有数据，才会继续
    condition_.wait(lock, [this] { return closed_ || !messages_.empty(); });
    if (messages_.empty()) {
        return false;
    }
    message = std::move(messages_.front());
    messages_.pop_front();
    bytes_ -= message.wire.size();
    return true;
}

bool OutboundQueue::try_pop(OutboundMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (messages_.empty()) {
        return false;
    }
    message = std::move(messages_.front());
    messages_.pop_front();
    bytes_ -= message.wire.size();
    return true;
}

bool OutboundQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.empty();
}

bool OutboundQueue::take_for_send(OutboundMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sending_) {
        throw std::logic_error("no current send");
    }
    bytes_ -= sending_bytes_;
    sending_bytes_ = 0;
    sending_ = false;
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
