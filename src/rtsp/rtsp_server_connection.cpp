#include "rtsp/rtsp_server_connection.h"

#include "media/media_session.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

std::atomic<std::uint64_t> next_session_id{1};
std::atomic<std::uint32_t> next_ssrc{1};

std::uint32_t make_ssrc() {
    std::uint32_t value = next_ssrc.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        value = next_ssrc.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

const SessionDescription& require_description(const std::shared_ptr<MediaSession>& media) {
    if (!media) {
        throw std::invalid_argument("connection requires a media session");
    }
    return media->description();
}

}  // namespace

RtspServerConnection::RtspServerConnection(std::shared_ptr<MediaSession> media_session)
    : media_session_(std::move(media_session)),
      session_id_(std::to_string(next_session_id.fetch_add(1, std::memory_order_relaxed))),
      outbound_queue_(std::make_shared<net::OutboundQueue>()),
      session_(session_id_, require_description(media_session_)) {}

RtspServerConnection::~RtspServerConnection() {
    close();
}

bool RtspServerConnection::accept_from(net::TcpSocket& listener) {
    if (!listener.try_accept(socket_)) {
        return false;
    }
    last_read_ = last_progress_ = Clock::now();
    return true;
}

bool RtspServerConnection::wants_read() const {
    return state_ == State::Open;
}

bool RtspServerConnection::wants_write() const {
    return state_ != State::Closed && (has_sending_ || !outbound_queue_->empty());
}

bool RtspServerConnection::finished() const {
    return state_ == State::Closed;
}

SOCKET RtspServerConnection::native_handle() const {
    return socket_.native_handle();
}

const std::string& RtspServerConnection::session_id() const {
    return session_id_;
}

void RtspServerConnection::close() {
    if (state_ == State::Closed) {
        return;
    }
    if (subscription_) {
        subscription_->cancel();
    }
    outbound_queue_->close();
    if (has_sending_) {
        outbound_queue_->complete_send();
        has_sending_ = false;
        sending_.wire.clear();
    }
    socket_.close();
    state_ = State::Closed;
}

void RtspServerConnection::begin_drain() {
    if (state_ != State::Open) {
        return;
    }
    if (subscription_) {
        subscription_->cancel();
    }
    outbound_queue_->close();
    state_ = State::Draining;
}

void RtspServerConnection::fail(const std::exception& error) {
    std::cerr << "Session " << session_id_ << " error: " << error.what() << '\n';
    close();
}

void RtspServerConnection::handle_item(const RtspStreamItem& item) {
    if (item.is_interleaved) {
        if (session_.state() == SessionInit || !session_.has_channel(item.channel)) {
            throw std::runtime_error("frame arrived before SETUP or on an unnegotiated channel");
        }
        return;  // RTCP 尚未实现，只验证已协商通道。
    }
    const RtspMessage& request = item.message;
    if (!request.is_request) {
        throw std::runtime_error("server expected an RTSP request");
    }
    std::cout << "Session " << session_id_ << " request: " << request.method
              << ", CSeq=" << request.header("CSeq") << std::endl;

    RtspServerSession next_session = session_;
    RtspMessage response = next_session.handle(request);
    bool response_queued = false;
    bool commit = true;
    if (request.method == "PLAY" && response.status_code == 200) {
        auto candidate = std::make_shared<MediaSubscription>(outbound_queue_);
        const auto& description = media_session_->description();
        const auto& tracks = next_session.tracks();
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].setup) {
                candidate->add_track(i, description.tracks[i], tracks[i].transport, 100, make_ssrc());
            }
        }
        const auto result = media_session_->subscribe(
            candidate, serialize_rtsp_message(response), false);
        if (result == SubscribeResult::Started) {
            subscription_ = std::move(candidate);
            response_queued = true;
        } else if (result == SubscribeResult::QueueClosed) {
            throw std::runtime_error("outbound queue is full or closed");
        } else {
            commit = false;
            response.status_code = result == SubscribeResult::InvalidRange ? 457 : 455;
            response.reason = result == SubscribeResult::InvalidRange
                ? "Invalid Range" : "Method Not Valid in This State";
        }
    }
    if (next_session.state() == SessionClosed && subscription_) {
        subscription_->cancel();  // 先停止生产，再把 TEARDOWN 响应排在已有媒体之后。
    }
    if (!response_queued && !outbound_queue_->push(
            {net::OutboundKind::RtspResponse, serialize_rtsp_message(response)})) {
        throw std::runtime_error("outbound queue is full or closed");
    }
    if (commit) {
        session_ = std::move(next_session);
    }
    std::cout << "Session " << session_id_ << " response: " << response.status_code
              << ' ' << response.reason << std::endl;
    if (session_.state() == SessionClosed) {
        begin_drain();
    }
}

void RtspServerConnection::on_readable() {
    if (!wants_read()) {
        return;
    }
    try {
        char buffer[4096];
        std::size_t budget = 64 * 1024;
        while (budget != 0 && wants_read()) {
            const auto result = socket_.receive_some(buffer, std::min(budget, sizeof(buffer)));
            if (result.status == net::IoStatus::WouldBlock) {
                return;
            }
            if (result.status == net::IoStatus::Eof) {
                parser_.finish();  // 所有已收完整消息已处理；半条消息属于截断。
                begin_drain();
                return;
            }
            last_read_ = Clock::now();
            budget -= result.bytes;
            parser_.append(buffer, result.bytes);
            RtspStreamItem item;
            while (wants_read() && parser_.next(item)) {
                handle_item(item);
            }
        }
    } catch (const std::exception& error) {
        fail(error);
    }
}

void RtspServerConnection::on_writable() {
    if (state_ == State::Closed) {
        return;
    }
    try {
        std::size_t budget = 64 * 1024;
        while (budget != 0) {
            if (!has_sending_) {
                if (!outbound_queue_->take_for_send(sending_)) {
                    break;
                }
                has_sending_ = true;
                send_offset_ = 0;
            }
            const auto remaining = sending_.wire.size() - send_offset_;
            if (remaining != 0) {
                const auto result = socket_.send_some(
                    sending_.wire.data() + send_offset_, std::min(budget, remaining));
                if (result.status == net::IoStatus::WouldBlock) {
                    break;
                }
                send_offset_ += result.bytes;
                budget -= result.bytes;
                last_progress_ = Clock::now();
            }
            if (send_offset_ == sending_.wire.size()) {
                outbound_queue_->complete_send();
                has_sending_ = false;
                sending_.wire.clear();
            }
        }
        if (!wants_write()) {
            waiting_write_ = false;
            if (state_ == State::Draining) {
                socket_.shutdown_send();
                close();
            }
        }
    } catch (const std::exception& error) {
        fail(error);
    }
}

void RtspServerConnection::check_timeout(Clock::time_point now) {
    if (state_ == State::Closed) {
        return;
    }
    // 如果subscription_存在并且不活跃
    if (subscription_ && subscription_->finished()) {
        if (subscription_->failed()) {
            close();
            return;
        }
        // 停止继续生产和接收新请求，把已有输出发完后再关闭。
        begin_drain();
    }
    if (state_ == State::Draining && !wants_write()) {
        try {
            socket_.shutdown_send();
        } catch (const std::exception& error) {
            fail(error);
            return;
        }
        close();
        return;
    }
    if (wants_write()) {
        // 第一次有数据到
        if (!waiting_write_) {
            waiting_write_ = true;
            last_progress_ = now;
        } else if (now - last_progress_ >= std::chrono::seconds(2)) {
            close();  // 代替阻塞发送超时，EOF 后也不无限等待慢客户端。
        }
        // 数据全部发完
    } else {
        waiting_write_ = false;
    }
    // 仅控制阶段执行空闲回收；正常播放不依赖客户端定期发送 RTSP 请求。
    if (state_ == State::Open && session_.state() != SessionPlaying
        && now - last_read_ >= std::chrono::seconds(60)) {
        close();
    }
}

}  // namespace rtsp
