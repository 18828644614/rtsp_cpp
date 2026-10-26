#include "rtsp/rtsp_server_connection.h"

#include "media/media_session.h"
#include "rtp/rtp_sender.h"
#include "rtsp/media_transport.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

std::atomic<std::uint64_t> next_session_id{1};
std::atomic<std::uint32_t> next_ssrc{1};

std::string make_session_id() {
    // 原子的把 next_session_id + 1,返回加之前的旧值(第一次返回1，第二次返回2)
    return std::to_string(next_session_id.fetch_add(1, std::memory_order_relaxed));
}

std::uint32_t make_ssrc() {
    std::uint32_t value = next_ssrc.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        value = next_ssrc.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

}  // namespace

RtspServerConnection::RtspServerConnection(std::shared_ptr<MediaSession> media_session)
    : media_session_(std::move(media_session)), session_id_(make_session_id()),
      outbound_queue_(std::make_shared<net::OutboundQueue>()) {
    if (!media_session_) {
        throw std::invalid_argument("connection requires a media session");
    }
}

RtspServerConnection::~RtspServerConnection() {
    stop();
    join();
}

void RtspServerConnection::accept_from(net::TcpSocket& listener) {
    if (worker_.joinable()) {
        throw std::logic_error("connection has already started");
    }
    listener.accept_client(socket_);
}

void RtspServerConnection::start() {
    if (!socket_.is_open()) {
        throw std::logic_error("connection socket is not open");
    }
    if (worker_.joinable()) {
        throw std::logic_error("connection has already started");
    }
    // 即使 EOF 时队列尚未达到上限，也不能让不读数据的客户端无限占用发送线程。
    socket_.set_send_timeout(2000);
    sender_ = std::thread(&RtspServerConnection::send_loop, this);
    try {
        // 接收并处理某一个客户端的 RTSP 请求，管理这个客户端的播放订阅，直到连接结束
        worker_ = std::thread(&RtspServerConnection::run, this);
    } catch (...) {
        stop_requested_.store(true, std::memory_order_release);
        outbound_queue_->close();
        socket_.shutdown();
        sender_.join();
        socket_.close();
        throw;
    }
}

void RtspServerConnection::stop() {
    stop_requested_.store(true, std::memory_order_release);
    outbound_queue_->close();
    socket_.shutdown();
}

void RtspServerConnection::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (sender_.joinable()) {
        sender_.join();
    }
    socket_.close();
}

bool RtspServerConnection::finished() const {
    return finished_.load(std::memory_order_acquire)
           && sender_finished_.load(std::memory_order_acquire);
}

const std::string& RtspServerConnection::session_id() const {
    return session_id_;
}

// 连接只处理协议与订阅生命周期；读取文件和媒体计时由 MediaSession 统一负责。
void RtspServerConnection::run() {
    std::shared_ptr<MediaSubscription> subscription;
    try {
        const SessionDescription& description = media_session_->description();
        RtspServerSession session(session_id_, description);
        RtspStreamParser parser;
        RtspStreamItem item;
        char buffer[4096];

        while (!stop_requested_.load(std::memory_order_acquire)
               && session.state() != SessionClosed) {
            if (subscription && subscription->finished()) {
                if (subscription->failed()) {
                    throw std::runtime_error("media subscription failed or outbound queue is full");
                }
                break;
            }
            // 定期检查媒体结束和入队失败，避免一直阻塞在 receive 中。
            if (!socket_.wait_readable(50)) {
                continue;
            }
            const std::size_t count = socket_.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();
                break;
            }
            parser.append(buffer, count);
            while (parser.next(item)) {
                if (item.is_interleaved) {
                    if (session.state() == SessionInit) {
                        throw std::runtime_error("unexpected media frame before SETUP");
                    }
                    if (!session.has_channel(item.channel)) {
                        throw std::runtime_error("frame arrived on an unnegotiated channel");
                    }
                    continue;
                }

                const RtspMessage& request = item.message;
                if (!request.is_request) {
                    throw std::runtime_error("server expected an RTSP request");
                }
                std::cout << "Session " << session_id_ << " request: " << request.method
                          << ", CSeq=" << request.header("CSeq") << std::endl;

                // 先在副本上校验；订阅失败不能把真实会话留在 Playing 状态。
                RtspServerSession next_session = session;
                RtspMessage response = next_session.handle(request);
                bool response_queued = false;
                bool commit = true;
                if (request.method == "PLAY" && response.status_code == 200) {
                    // 创建媒体订阅 
                    auto candidate = std::make_shared<MediaSubscription>(outbound_queue_);
                    const std::vector<TrackState>& tracks = next_session.tracks();
                    for (std::size_t i = 0; i < tracks.size(); ++i) {
                        if (tracks[i].setup) {
                            candidate->add_track(i, description.tracks[i], tracks[i].transport,
                                                 100, make_ssrc());
                        }
                    }
                    
                    // candidate	刚创建的客户端订阅
                    // serialize_rtsp_message(response)	把 PLAY 响应转换成可发送的字符串
                    // RtspServerSession 已经拒绝了非零 seek；ffplay 的 npt=0.000- 是
                    // “从零开始”的兼容写法，但晚加入客户端仍应加入共享进度。
                    const auto result = media_session_->subscribe(
                        candidate, serialize_rtsp_message(response), false);

                    if (result == SubscribeResult::Started) {
                        subscription = std::move(candidate);
                        response_queued = true;
                    } else if (result == SubscribeResult::QueueClosed) {
                        throw std::runtime_error("outbound queue is full or closed");

                    // 资源不允许订阅：修改响应，保留原来的状态
                    // InvalidRange：共享进度已开始，无法满足从头播放	457 Invalid Range
                    // Ended：媒体资源已结束或正在停止	455 Method Not Valid in This State
                    } else {
                        commit = false;
                        response.status_code = result == SubscribeResult::InvalidRange ? 457 : 455;
                        response.reason = result == SubscribeResult::InvalidRange
                            ? "Invalid Range" : "Method Not Valid in This State";
                        std::cout << "Media PLAY rejected: "
                                  << (result == SubscribeResult::Ended ? "resource ended" : "shared progress already started")
                                  << std::endl;
                    }
                }
                if (next_session.state() == SessionClosed && subscription) {
                    subscription->cancel();  // 返回后不会再入队 RTP，随后才能入队 TEARDOWN 响应。
                }
                
                // PLAY 订阅成功：前面已经入队，这里跳过。
                // PLAY 被拒绝：这里把修改后的错误响应入队。
                // SETUP、TEARDOWN 等其他请求：这里把对应响应入队。
                if (!response_queued && !outbound_queue_->push(
                        {net::OutboundKind::RtspResponse, serialize_rtsp_message(response)})) {
                    throw std::runtime_error("outbound queue is full or closed");
                }
                if (commit) {
                    session = next_session;
                }
                std::cout << "Session " << session_id_ << " response: " << response.status_code
                          << ' ' << response.reason << std::endl;
                if (session.state() == SessionClosed) {
                    break;
                }
            }
        }
    } catch (const std::exception& error) {
        const bool was_stopping = stop_requested_.exchange(true, std::memory_order_acq_rel);
        socket_.shutdown();
        if (!was_stopping) {
            std::cerr << "Session " << session_id_ << " error: " << error.what() << '\n';
        }
    }

    if (subscription) {
        subscription->cancel();
    }
    outbound_queue_->close();
    finished_.store(true, std::memory_order_release);
}

void RtspServerConnection::send_loop() {
    try {
        net::OutboundMessage message;
        // pop 会阻塞等待，不会导致空转
        while (outbound_queue_->pop(message)) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            // rtsp 响应， rtp 视频包， rtcp 数据
            socket_.send_all(message.wire.data(), message.wire.size());
        }

        // 如果发送队列正常结束，并且没有人为停止或发生错误，就通知客户端：我不会再发送数据了。
        // pop 会确保如果队列为打开状态，队列中一定有数据
        if (!stop_requested_.load(std::memory_order_acquire) && socket_.is_open()) {
            socket_.shutdown_send();
        }
    } catch (const std::exception& error) {
        stop_requested_.store(true, std::memory_order_release);
        outbound_queue_->close();
        if (socket_.is_open()) {
            std::cerr << "Session " << session_id_ << " send error: " << error.what() << '\n';
        }
    }
    // 发送已结束，也通知接收线程退出，避免正常 EOF 的 shutdown 被当作接收错误。
    stop_requested_.store(true, std::memory_order_release);
    // 仅中断收发，不修改句柄。管理线程 join 两个线程后才真正 close。
    socket_.shutdown();
    sender_finished_.store(true, std::memory_order_release);
}

}  // namespace rtsp
