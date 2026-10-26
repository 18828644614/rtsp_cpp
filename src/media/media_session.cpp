#include "media/media_session.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {

MediaSubscription::MediaSubscription(std::shared_ptr<net::OutboundQueue> queue,
                                     MediaTransport transport, int payload_type,
                                     std::uint16_t sequence, std::uint32_t ssrc)
    : queue_(std::move(queue)), sender_(std::move(transport), payload_type, sequence, ssrc) {
    if (!queue_) {
        throw std::invalid_argument("subscription requires an outbound queue");
    }
}

MediaSubscription::~MediaSubscription() {
    cancel();
}

bool MediaSubscription::start(const std::string& play_response) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_->push({net::OutboundKind::RtspResponse, play_response})) {
        return false;
    }
    active_ = true;
    return true;
}

void MediaSubscription::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
}

bool MediaSubscription::finished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_;
}

bool MediaSubscription::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

void MediaSubscription::deliver(const std::shared_ptr<const H264PayloadUnit>& unit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || (waiting_for_idr_ && !unit->access_unit->is_keyframe)) {
        return;
    }
    waiting_for_idr_ = false;
    try {
        if (sender_.send_payloads(*unit, *queue_)) {
            return;
        }
    } catch (const std::exception& error) {
        std::cerr << "Subscription send error: " << error.what() << '\n';
    }
    // 入队失败只结束这一个订阅，连接线程负责中断 Socket。
    failed_ = true;
    active_ = false;
    queue_->close();
}

void MediaSubscription::finish(bool failed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;  // 已取消的订阅由连接自行关闭队列，不能抢在 TEARDOWN 响应前关闭。
    }
    failed_ = failed_ || failed;
    if (waiting_for_idr_ && !failed) {
        std::cout << "Subscription ended before an IDR became available" << std::endl;
    }
    active_ = false;
    // 正常 EOF 允许发送线程排空队列；异常由连接线程中断。
    queue_->close();
}

MediaSession::MediaSession(const std::string& input_path) : source_(input_path) {
    worker_ = std::thread(&MediaSession::run, this);
}

MediaSession::~MediaSession() {
    stop();
}

const SessionDescription& MediaSession::description() const {
    return source_.description();
}

SubscribeResult MediaSession::subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                                        const std::string& play_response, bool from_start) {
    if (!subscription) {
        throw std::invalid_argument("expected a media subscription");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended_ || stopping_) {
        return SubscribeResult::Ended;
    }
    if (from_start && started_) {
        return SubscribeResult::InvalidRange;
    }
    // 先注册，再入队；分配失败时不会先发出成功响应。
    subscribers_.push_back(subscription);
    if (!subscription->start(play_response)) {
        subscribers_.pop_back();
        return SubscribeResult::QueueClosed;
    }
    condition_.notify_one();
    return SubscribeResult::Started;
}

bool MediaSession::ended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_;
}

void MediaSession::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MediaSession::finish(bool failed) {
    std::vector<std::weak_ptr<MediaSubscription>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
        subscribers.swap(subscribers_);
    }
    for (const auto& weak : subscribers) {
        if (auto subscriber = weak.lock()) {
            subscriber->finish(failed);
        }
    }
}

void MediaSession::run() {
    using Clock = std::chrono::steady_clock;
    auto next_send = Clock::now();
    try {
        for (;;) {
            std::vector<std::shared_ptr<MediaSubscription>> subscribers;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 取消的订阅和已销毁的连接都不再推动媒体进度。
                auto it = subscribers_.begin();
                while (it != subscribers_.end()) {
                    auto subscriber = it->lock();
                    if (!subscriber || subscriber->finished()) {
                        it = subscribers_.erase(it);
                    } else {
                        subscribers.push_back(std::move(subscriber));
                        ++it;
                    }
                }
                if (stopping_) {
                    break;
                }
                if (subscribers.empty()) {
                    condition_.wait(lock, [this] { return stopping_ || !subscribers_.empty(); });
                    // 暂停期间不消耗文件；恢复时不补发暂停期间的帧。
                    next_send = Clock::now();
                    continue;
                }
                if (Clock::now() < next_send) {
                    condition_.wait_until(lock, next_send);
                    continue;  // 重新获取快照，让等待期间加入的客户端也收到下一帧。
                }
                started_ = true;
            }

            const auto frame = source_.read();
            if (!frame) {
                break;
            }
            const auto payloads = packetizer_.packetize_access_unit(frame);
            // 不持有订阅表锁，不访问 Socket，只执行有界队列的非阻塞入队。
            for (const auto& subscriber : subscribers) {
                subscriber->deliver(payloads);
            }
            if (source_.ended()) {
                break;
            }
            next_send += std::chrono::microseconds(
                static_cast<std::uint64_t>(frame->duration) * 1000000 / 90000);
        }
        finish(false);
    } catch (const std::exception& error) {
        std::cerr << "MediaSession error: " << error.what() << '\n';
        finish(true);
    }
}

}  // namespace rtsp
