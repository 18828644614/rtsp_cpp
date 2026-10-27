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

RtspServerConnection::RtspServerConnection(std::shared_ptr<MediaSession> media_session,
                                           RuntimeOptions options)
    : media_session_(std::move(media_session)),
      session_id_(std::to_string(next_session_id.fetch_add(1, std::memory_order_relaxed))),
      outbound_queue_(std::make_shared<net::OutboundQueue>()),
      session_(session_id_, require_description(media_session_), options.session_timeout_seconds),
      options_(options) {
    if (options_.rtcp_interval.count() <= 0 || options_.control_idle_timeout.count() <= 0
        || options_.send_stall_timeout.count() <= 0 || options_.drain_timeout.count() <= 0) {
        throw std::invalid_argument("runtime timeouts must be positive");
    }
}

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

void RtspServerConnection::begin_drain(Clock::time_point now) {
    if (state_ != State::Open) {
        return;
    }
    if (subscription_) {
        subscription_->end();
    }
    outbound_queue_->close();
    state_ = State::Draining;
    drain_deadline_ = now + options_.drain_timeout;
}

void RtspServerConnection::start_shutdown() {
    begin_drain();
}

std::optional<RtspServerConnection::Clock::time_point> RtspServerConnection::next_deadline() const {
    if (state_ == State::Closed) { return std::nullopt; }
    auto deadline = state_ == State::Draining ? drain_deadline_ : session_deadline_;
    if (!deadline) { deadline = last_read_ + options_.control_idle_timeout; }
    if (waiting_write_) { deadline = std::min(*deadline, last_progress_ + options_.send_stall_timeout); }
    if (state_ == State::Open && subscription_) {
        if (const auto report = subscription_->next_rtcp_deadline()) {
            deadline = std::min(*deadline, *report);
        }
    }
    return deadline;
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
        for (const auto& track : session_.tracks()) {
            if (track.setup && item.channel == static_cast<unsigned int>(track.transport.rtcp_channel)) {
                if (subscription_) {
                    if (subscription_->receive_rtcp(item.channel, item.payload)) {
                        session_deadline_ = Clock::now() + std::chrono::seconds(options_.session_timeout_seconds);
                    }
                } else {
                    parse_rtcp_compound(item.payload);
                }
                return;
            }
        }
        throw std::runtime_error("server accepts RTCP feedback, not incoming RTP media");
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
        const auto base = request.uri.substr(0, request.uri.find('/', 7)) + "/live/";
        response.set_header("RTP-Info", candidate->rtp_info(base));
        const auto result = media_session_->subscribe(
            candidate, serialize_rtsp_message(response), false);
        if (result == SubscribeResult::Started) {
            subscription_ = std::move(candidate);
            response_queued = true;
        } else if (result == SubscribeResult::QueueClosed) {
            throw std::runtime_error("outbound queue is full or closed");
        } else {
            commit = false;
            response.headers.erase(std::remove_if(response.headers.begin(), response.headers.end(),
                [](const RtspHeader& header) { return header.name == "RTP-Info"; }), response.headers.end());
            response.status_code = result == SubscribeResult::InvalidRange ? 457 : 455;
            response.reason = result == SubscribeResult::InvalidRange
                ? "Invalid Range" : "Method Not Valid in This State";
        }
    }
    if (next_session.state() == SessionClosed && subscription_) {
        subscription_->end();  // 停止生产，最终 RTCP 和 TEARDOWN 响应排在已有媒体之后。
    }
    if (!response_queued && !outbound_queue_->push(
            {net::OutboundKind::RtspResponse, serialize_rtsp_message(response)})) {
        throw std::runtime_error("outbound queue is full or closed");
    }
    if (commit) {
        session_ = std::move(next_session);
        if (response.status_code == 200 && (request.method == "SETUP"
            || session_id_from_header(request.header("Session")) == session_id_)) {
            session_deadline_ = Clock::now() + std::chrono::seconds(options_.session_timeout_seconds);
        }
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
                if (sending_.kind == net::OutboundKind::Rtcp && subscription_) {
                    auto wire = subscription_->prepare_rtcp(sending_, media_session_->clock(), Clock::now());
                    if (wire.size() != sending_.wire.size()) {
                        throw std::logic_error("RTCP snapshot changed reserved queue size");
                    }
                    sending_.wire = std::move(wire);
                }
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
                if (subscription_) { subscription_->on_sent(sending_); }
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
        begin_drain(now);
    }
    if (state_ == State::Open && session_deadline_ && now >= *session_deadline_) {
        std::cout << "Session " << session_id_ << " expired" << std::endl;
        begin_drain(now);
    }
    if (state_ == State::Draining && now >= *drain_deadline_) {
        close();
        return;
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
        } else if (now - last_progress_ >= options_.send_stall_timeout) {
            close();  // 代替阻塞发送超时，EOF 后也不无限等待慢客户端。
            return;
        }
        // 数据全部发完
    } else {
        waiting_write_ = false;
    }
    // SETUP 后只由有效 Session 请求或匹配的 RR 续期，发送媒体不刷新期限。
    if (state_ == State::Open && !session_deadline_
        && now - last_read_ >= options_.control_idle_timeout) {
        close();
        return;
    }
    if (state_ == State::Open && subscription_) {
        subscription_->tick_rtcp(now, options_.rtcp_interval);
        if (subscription_->failed()) { close(); }
    }
}

}  // namespace rtsp
