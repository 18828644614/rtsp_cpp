#include "rtsp/rtsp_server_connection.h"

#include "media/h264_file_reader.h"
#include "rtp/rtp_sender.h"
#include "rtsp/media_transport.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <atomic>
#include <chrono>
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

RtspServerConnection::RtspServerConnection(std::string input_path)
    : input_path_(std::move(input_path)), session_id_(make_session_id()) {}

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
    sender_ = std::thread(&RtspServerConnection::send_loop, this);
    try {
        worker_ = std::thread(&RtspServerConnection::run, this);
    } catch (...) {
        stop_requested_.store(true, std::memory_order_release);
        outbound_queue_.close();
        socket_.close();
        sender_.join();
        throw;
    }
}

void RtspServerConnection::stop() {
    stop_requested_.store(true, std::memory_order_release);
    outbound_queue_.close();
    socket_.close();
}

void RtspServerConnection::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (sender_.joinable()) {
        sender_.join();
    }
}

bool RtspServerConnection::finished() const {
    return finished_.load(std::memory_order_acquire);
}

const std::string& RtspServerConnection::session_id() const {
    return session_id_;
}

// 读取视频文件；
// 接收客户端发送的 RTSP 请求；
// 解析 RTSP 消息；
// 管理 RTSP 会话状态；
// 生成 RTSP 响应；
// 生成 RTP 数据；
// 把待发送数据放入发送队列；
// 在停止、断开或出错时清理资源。
void RtspServerConnection::run() {
    try {
        H264FileReader reader;
        reader.open(input_path_);

        // 根据 H264 的 SPS/PPS 生成 SDP 信息
        // 为后面的 DESCRIBE 响应准备数据
        SessionDescription description;
        description.video.fmtp = make_h264_fmtp(reader.sps(), reader.pps());

        // 创建 RTSP 会话和流解析器
        RtspServerSession session(session_id_, description);
        RtspStreamParser parser;
        RtspStreamItem item; // 解析结果的临时保存对象
        char buffer[4096];
        std::uint32_t timestamp = 0;
        std::unique_ptr<RtpSender> rtp_sender;
        std::size_t sent_frames = 0;
        std::chrono::steady_clock::time_point next_send;

        while (!stop_requested_.load(std::memory_order_acquire)
               && session.state() != SessionClosed) {
            if (session.state() == SessionPlaying) {
                const auto now = std::chrono::steady_clock::now();
                int wait_ms = 0;
                if (now < next_send) {
                    wait_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(next_send - now).count()) + 1;
                }

                // Socket 上有客户端数据 或者 等待时间超时，false: 等待超时，发送视频帧
                if (!socket_.wait_readable(wait_ms)) {
                    H264AccessUnit frame;
                    if (!reader.read(frame)) {
                        std::cout << "Session " << session_id_ << " completed, access units="
                                  << sent_frames << std::endl;
                        break;
                    }

                    // 把 H264 帧封装成 可 tcp 直接发送的 RTP 并放入发送队列
                    if (!rtp_sender
                        || !rtp_sender->send_access_unit(frame, timestamp, outbound_queue_)) {
                        throw std::runtime_error("outbound queue is full or closed");
                    }
                    timestamp += 3600;
                    ++sent_frames;
                    next_send += std::chrono::milliseconds(40);
                    continue;
                }
            }

            const std::size_t count = socket_.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();
                break;
            }
            parser.append(buffer, count);
            while (parser.next(item)) {
                // 是 tcp 数据交织帧 
                if (item.is_interleaved) {
                    if (session.state() == SessionInit) {
                        throw std::runtime_error("unexpected media frame before SETUP");
                    }
                    const TcpTransport transport = session.transport();
                    if (item.channel != static_cast<unsigned int>(transport.rtp_channel)
                        && item.channel != static_cast<unsigned int>(transport.rtcp_channel)) {
                        throw std::runtime_error("frame arrived on an unnegotiated channel");
                    }
                    continue;
                }

                // rtsp 请求：
                const RtspMessage& request = item.message;
                if (!request.is_request) {
                    throw std::runtime_error("server expected an RTSP request");
                }
                std::cout << "Session " << session_id_ << " request: " << request.method
                          << ", CSeq=" << request.header("CSeq") << std::endl;
                const RtspMessage response = session.handle(request);
                net::OutboundMessage message;
                message.kind = net::OutboundKind::RtspResponse;
                message.wire = serialize_rtsp_message(response);
                if (!outbound_queue_.push(std::move(message))) {
                    throw std::runtime_error("outbound queue is full or closed");
                }
                std::cout << "Session " << session_id_ << " response: " << response.status_code
                          << ' ' << response.reason << std::endl;
                if (request.method == "PLAY" && response.status_code == 200) {
                    rtp_sender = std::make_unique<RtpSender>(
                        MediaTransport(session.transport()), description.video.payload_type,
                        static_cast<std::uint16_t>(100), make_ssrc());
                    next_send = std::chrono::steady_clock::now();
                }
                if (session.state() == SessionClosed) {
                    break;
                }
            }
        }
    } catch (const std::exception& error) {
        const bool was_stopping = stop_requested_.exchange(true, std::memory_order_acq_rel);
        outbound_queue_.close();
        socket_.close();
        if (!was_stopping) {
            std::cerr << "Session " << session_id_ << " error: " << error.what() << '\n';
        }
    }

    outbound_queue_.close();
    finished_.store(true, std::memory_order_release);
}

void RtspServerConnection::send_loop() {
    try {
        net::OutboundMessage message;
        // pop 会阻塞等待，不会导致空转
        while (outbound_queue_.pop(message)) {
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
        outbound_queue_.close();
        if (socket_.is_open()) {
            std::cerr << "Session " << session_id_ << " send error: " << error.what() << '\n';
        }
    }
    socket_.close();
}

}  // namespace rtsp
