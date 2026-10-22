#include "rtsp.h"

#include <arpa/inet.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <set>
#include <thread>

#include "app/auth.h"
#include "core/log.h"
#include "core/util.h"
#include "http.h"
#include "loop.h"
#include "media/stream.h"

namespace rtsp {

static const char *TAG = "rtsp";
static const size_t kMtu = 1400;
// Video queued for a viewer is delay: whatever sits in a queue is shown that much later, for the rest of the
// session. A slow viewer skips to a fresh keyframe once more than ~0.4 s is waiting, instead of building up seconds.
static const size_t kMinBacklog = 96 * 1024;
static std::string g_auth = "digest";
static Listener g_listener;
static int g_port = 554;

class RtspConn;
static std::set<RtspConn *> g_conns;

static int stream_for_path(const std::string &path, const std::string &query) {
    std::string p = to_lower(path);
    if (p.find("sub") != std::string::npos || ends_with(p, "/1") || ends_with(p, "102") || ends_with(p, "stream2") ||
        ends_with(p, "/live/1") || query.find("subtype=1") != std::string::npos)
        return 1;
    return 0;
}

class RtspConn : public Conn {
public:
    RtspConn(int fd, const std::string &peer) : Conn(fd, peer) {
        g_conns.insert(this);
        // the kernel queue counts too (Linux auto-tunes it to megabytes = seconds of video): keep it small
        int sndbuf = 128 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        ssrc_ = (uint32_t)strtoul(random_token(4).c_str(), nullptr, 16);
        session_id_ = random_token(8);
    }
    ~RtspConn() override {
        g_conns.erase(this);
        stop_stream();
        if (udp_fd_ >= 0) ::close(udp_fd_);
    }
    bool playing() const { return playing_; }
    bool idle_exempt() const override { return playing_; }
    void kick() { close(); }

protected:
    void on_data(std::string &in) override;
    void on_close() override { stop_stream(); }
    void on_drained() override {}

private:
    void handle(const std::string &method, const std::string &uri, std::map<std::string, std::string> &h,
                const std::string &body);
    void reply(int code, const std::string &cseq, const std::string &extra = "", const std::string &body = "");
    bool authorized(const std::string &method, std::map<std::string, std::string> &h, const std::string &cseq);
    std::string sdp(Stream *s, const std::string &uri);
    void start_stream();
    void stop_stream();
    void on_packet(const Packet &p);
    size_t backlog() const;
    void send_rtp(const uint8_t *payload_hdr, size_t hdr_len, const uint8_t *data, size_t len, bool marker,
                  uint32_t ts);
    void send_rtcp_sr(uint32_t ts);

    int stream_idx_ = 0;
    Stream *stream_ = nullptr;
    int sub_id_ = 0;
    int64_t gen_ = -1;
    bool playing_ = false, tcp_ = true, need_key_ = true, authed_ = false;
    int ch_rtp_ = 0, ch_rtcp_ = 1;
    int udp_fd_ = -1;
    sockaddr_in udp_dst_{}, udp_rtcp_dst_{};
    uint16_t seq_ = 0;
    uint32_t ssrc_ = 0, octets_ = 0, packets_ = 0;
    int64_t last_sr_ = 0, ts_base_us_ = -1, last_skip_idr_ = 0;
    bool describe_waited_ = false;
    std::string session_id_;
};

static std::string header_get(std::map<std::string, std::string> &h, const char *k) {
    auto it = h.find(k);
    return it == h.end() ? "" : it->second;
}

void RtspConn::on_data(std::string &in) {
    while (!closed()) {
        // RTCP from interleaved clients ('$' framing): skip
        if (!in.empty() && in[0] == '$') {
            if (in.size() < 4) return;
            size_t len = ((uint8_t)in[2] << 8) | (uint8_t)in[3];
            if (in.size() < 4 + len) return;
            in.erase(0, 4 + len);
            continue;
        }
        size_t end = in.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (in.size() > 8192) close();
            return;
        }
        std::string head = in.substr(0, end);
        auto lines = split(head, '\n');
        std::map<std::string, std::string> h;
        for (size_t i = 1; i < lines.size(); ++i) {
            size_t c = lines[i].find(':');
            if (c != std::string::npos) h[to_lower(trim(lines[i].substr(0, c)))] = trim(lines[i].substr(c + 1));
        }
        size_t blen = atoi(header_get(h, "content-length").c_str());
        if (in.size() < end + 4 + blen) return;
        std::string body = in.substr(end + 4, blen);
        in.erase(0, end + 4 + blen);
        auto first = split(trim(lines[0]), ' ');
        if (first.size() < 3) {
            close();
            return;
        }
        handle(first[0], first[1], h, body);
    }
}

void RtspConn::reply(int code, const std::string &cseq, const std::string &extra, const std::string &body) {
    const char *txt = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : code == 454 ? "Session Not Found"
                    : code == 461 ? "Unsupported Transport" : code == 455 ? "Method Not Valid in This State" : "Bad Request";
    std::string r = "RTSP/1.0 " + std::to_string(code) + " " + txt + "\r\nCSeq: " + cseq +
                    "\r\nServer: PicoCam\r\nDate: " + format_time(time(nullptr), "%a, %d %b %Y %H:%M:%S GMT", false) + "\r\n" + extra;
    if (!body.empty()) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n" + body;
    send(std::move(r));
}

bool RtspConn::authorized(const std::string &method, std::map<std::string, std::string> &h, const std::string &cseq) {
    if (authed_) return true;
    if (g_auth == "none") return authed_ = true;
    if (!auth::activated()) {
        reply(401, cseq, "WWW-Authenticate: " + digest_challenge() + "\r\n");
        return false;
    }
    if (auth::locked_out(peer_)) {
        reply(401, cseq);
        return false;
    }
    std::string authz = header_get(h, "authorization");
    if (!authz.empty()) {
        std::string user;
        if (verify_authorization(authz, method, &user, g_auth == "basic") >= auth::VIEWER) {
            auth::note_success(peer_);
            return authed_ = true;
        }
        auth::note_failure(peer_);
    }
    std::string chal = g_auth == "basic" ? std::string("Basic realm=\"") + auth::realm() + "\"" : digest_challenge();
    reply(401, cseq, "WWW-Authenticate: " + chal + "\r\n");
    return false;
}

static std::string profile_level_id(const std::string &sps) {
    if (sps.size() < 4) return "42e01f";
    return hex((const uint8_t *)sps.data() + 1, 3);
}

std::string RtspConn::sdp(Stream *s, const std::string &uri) {
    std::string ip = iface_ipv4(primary_iface());
    auto ps = s->params();
    std::string o = "v=0\r\no=- " + std::to_string(wall_ms()) + " 1 IN IP4 " + ip +
                    "\r\ns=PicoCam\r\ni=" + (s->index() == 0 ? "main" : "sub") + "\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n" +
                    "a=range:npt=now-\r\nm=video 0 RTP/AVP 96\r\nb=AS:" + std::to_string(std::max(256, (int)s->kbps() * 2)) +
                    "\r\na=framerate:" + std::to_string(s->fps()) + "\r\n";
    if (s->codec() == Codec::H265) {
        o += "a=rtpmap:96 H265/90000\r\n";
        if (ps.valid())
            o += "a=fmtp:96 sprop-vps=" + base64_encode(ps.vps) + ";sprop-sps=" + base64_encode(ps.sps) +
                 ";sprop-pps=" + base64_encode(ps.pps) + "\r\n";
    } else {
        o += "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1";
        if (ps.valid())
            o += ";profile-level-id=" + profile_level_id(ps.sps) + ";sprop-parameter-sets=" + base64_encode(ps.sps) + "," +
                 base64_encode(ps.pps);
        o += "\r\n";
    }
    o += "a=control:" + uri + "/trackID=0\r\n";
    (void)uri;
    return o;
}

void RtspConn::handle(const std::string &method, const std::string &uri, std::map<std::string, std::string> &h,
                      const std::string &) {
    std::string cseq = header_get(h, "cseq");
    if (method == "OPTIONS") {
        reply(200, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
        return;
    }
    if (!authorized(method, h, cseq)) return;

    // rtsp://host:port/path?query
    std::string path = uri, query;
    size_t sch = path.find("://");
    if (sch != std::string::npos) {
        size_t sl = path.find('/', sch + 3);
        path = sl == std::string::npos ? "/" : path.substr(sl);
    }
    size_t q = path.find('?');
    if (q != std::string::npos) query = path.substr(q + 1), path = path.substr(0, q);

    if (method == "DESCRIBE") {
        stream_idx_ = stream_for_path(path, query);
        stream_ = stream_by_index(stream_idx_);
        // parameter sets come with keyframes; wait briefly for one if the encoder just started
        if (!stream_->params().valid() && !describe_waited_) {
            // wait for the first keyframe without blocking the event loop (it carries every viewer's video)
            describe_waited_ = true;
            if (stream_->request_idr) stream_->request_idr();
            std::weak_ptr<Conn> w = shared_from_this();
            Stream *st = stream_;
            auto hh = h;
            std::string m = method, u = uri;
            std::thread([w, st, hh, m, u]() mutable {
                for (int i = 0; i < 20 && !st->params().valid(); ++i) usleep(50 * 1000);
                g_loop.post([w, hh, m, u]() mutable {
                    if (auto c = w.lock())
                        if (!c->closed()) static_cast<RtspConn *>(c.get())->handle(m, u, hh, "");
                });
            }).detach();
            return;
        }
        describe_waited_ = false;
        if (!stream_->params().valid()) {
            reply(404, cseq);
            return;
        }
        std::string base = uri;
        if (!base.empty() && base.back() == '/') base.pop_back();
        reply(200, cseq, "Content-Base: " + base + "/\r\nContent-Type: application/sdp\r\n", sdp(stream_, base));
        return;
    }
    if (method == "SETUP") {
        if (!stream_) {
            stream_idx_ = stream_for_path(path, query);
            stream_ = stream_by_index(stream_idx_);
        }
        std::string tr = header_get(h, "transport");
        std::string resp_tr;
        if (tr.find("RTP/AVP/TCP") != std::string::npos || tr.find("interleaved") != std::string::npos) {
            tcp_ = true;
            size_t i = tr.find("interleaved=");
            if (i != std::string::npos) sscanf(tr.c_str() + i, "interleaved=%d-%d", &ch_rtp_, &ch_rtcp_);
            resp_tr = "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(ch_rtp_) + "-" + std::to_string(ch_rtcp_);
        } else {
            size_t i = tr.find("client_port=");
            if (i == std::string::npos || tr.find("multicast") != std::string::npos) {
                reply(461, cseq);
                return;
            }
            int p1 = 0, p2 = 0;
            sscanf(tr.c_str() + i, "client_port=%d-%d", &p1, &p2);
            if (!p2) p2 = p1 + 1;
            tcp_ = false;
            if (udp_fd_ < 0) udp_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            bind(udp_fd_, (sockaddr *)&local, sizeof(local));
            socklen_t ll = sizeof(local);
            getsockname(udp_fd_, (sockaddr *)&local, &ll);
            int sndbuf = 512 * 1024;
            setsockopt(udp_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            udp_dst_.sin_family = AF_INET;
            inet_pton(AF_INET, peer_.c_str(), &udp_dst_.sin_addr);
            udp_dst_.sin_port = htons(p1);
            udp_rtcp_dst_ = udp_dst_;
            udp_rtcp_dst_.sin_port = htons(p2);
            int sp = ntohs(local.sin_port);
            resp_tr = "RTP/AVP;unicast;client_port=" + std::to_string(p1) + "-" + std::to_string(p2) +
                      ";server_port=" + std::to_string(sp) + "-" + std::to_string(sp + 1);
        }
        char ss[16];
        snprintf(ss, sizeof(ss), "%08x", ssrc_);
        reply(200, cseq, "Transport: " + resp_tr + ";ssrc=" + ss + "\r\nSession: " + session_id_ + ";timeout=60\r\n");
        return;
    }
    if (method == "PLAY") {
        if (!stream_) {
            reply(455, cseq);
            return;
        }
        reply(200, cseq, "Session: " + session_id_ + "\r\nRange: npt=0.000-\r\nRTP-Info: url=" + uri + "/trackID=0;seq=" +
                             std::to_string(seq_) + "\r\n");
        start_stream();
        return;
    }
    if (method == "PAUSE") {
        stop_stream();
        reply(200, cseq, "Session: " + session_id_ + "\r\n");
        return;
    }
    if (method == "TEARDOWN") {
        reply(200, cseq, "Session: " + session_id_ + "\r\n");
        stop_stream();
        close_after_send();
        return;
    }
    if (method == "GET_PARAMETER" || method == "SET_PARAMETER") {
        reply(200, cseq, "Session: " + session_id_ + "\r\n");
        return;
    }
    reply(400, cseq);
}

void RtspConn::start_stream() {
    if (playing_) return;
    playing_ = true;
    need_key_ = true;
    gen_ = stream_->generation();
    if (tcp_) {
        // kernel send queue of ~0.25 s at this stream's rate (the kernel doubles the value): enough to ride out
        // Wi-Fi hiccups, too little to build up a delay
        int sndbuf = std::max(32 * 1024, std::min(256 * 1024, (int)(stream_->kbps() * 1000 / 8 * 0.25f / 2)));
        setsockopt(fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
    std::weak_ptr<Conn> self = shared_from_this();
    sub_id_ = stream_->subscribe([self](const Packet &p) {
        g_loop.post([self, p] {
            if (auto c = self.lock()) static_cast<RtspConn *>(c.get())->on_packet(p);
        });
    });
    // a fresh keyframe gets the picture up immediately instead of waiting up to a GOP
    if (stream_->request_idr) stream_->request_idr();
    LOGI(TAG, "%s playing %s over %s", peer_.c_str(), stream_idx_ ? "sub" : "main", tcp_ ? "TCP" : "UDP");
}

void RtspConn::stop_stream() {
    if (!playing_) return;
    playing_ = false;
    if (stream_ && sub_id_) stream_->unsubscribe(sub_id_);
    sub_id_ = 0;
}

void RtspConn::send_rtp(const uint8_t *ph, size_t phl, const uint8_t *d, size_t n, bool marker, uint32_t ts) {
    size_t plen = 12 + phl + n;
    std::string pkt;
    pkt.reserve(4 + plen);
    if (tcp_) {
        pkt += '$';
        pkt += (char)ch_rtp_;
        pkt += (char)(plen >> 8);
        pkt += (char)(plen & 0xff);
    }
    uint8_t h[12] = {0x80, (uint8_t)(96 | (marker ? 0x80 : 0)), (uint8_t)(seq_ >> 8), (uint8_t)seq_,
                     (uint8_t)(ts >> 24), (uint8_t)(ts >> 16), (uint8_t)(ts >> 8), (uint8_t)ts,
                     (uint8_t)(ssrc_ >> 24), (uint8_t)(ssrc_ >> 16), (uint8_t)(ssrc_ >> 8), (uint8_t)ssrc_};
    ++seq_;
    pkt.append((const char *)h, 12);
    pkt.append((const char *)ph, phl);
    pkt.append((const char *)d, n);
    ++packets_;
    octets_ += phl + n;
    if (tcp_) {
        send(std::move(pkt));
    } else {
        sendto(udp_fd_, pkt.data(), pkt.size(), MSG_DONTWAIT, (sockaddr *)&udp_dst_, sizeof(udp_dst_));
    }
}

void RtspConn::send_rtcp_sr(uint32_t ts) {
    // sender report: lets clients map RTP time to wall clock (A/V sync, NVR timestamps)
    int64_t ms = wall_ms();
    uint64_t ntp_sec = (uint64_t)(ms / 1000) + 2208988800ULL;
    uint64_t ntp_frac = (uint64_t)((ms % 1000) * 4294967.296);
    uint8_t r[28] = {0x80, 200, 0, 6};
    auto put32 = [&](int o, uint32_t v) {
        r[o] = v >> 24;
        r[o + 1] = v >> 16;
        r[o + 2] = v >> 8;
        r[o + 3] = v;
    };
    put32(4, ssrc_);
    put32(8, (uint32_t)ntp_sec);
    put32(12, (uint32_t)ntp_frac);
    put32(16, ts);
    put32(20, packets_);
    put32(24, octets_);
    if (tcp_) {
        std::string s;
        s += '$';
        s += (char)ch_rtcp_;
        s += (char)0;
        s += (char)28;
        s.append((const char *)r, 28);
        send(std::move(s));
    } else {
        sendto(udp_fd_, r, 28, MSG_DONTWAIT, (sockaddr *)&udp_rtcp_dst_, sizeof(udp_rtcp_dst_));
    }
}

// bytes waiting for this viewer: our queue plus what the kernel has not sent yet (both are delay)
size_t RtspConn::backlog() const {
    int outq = 0;
    if (tcp_ && ioctl(fd(), SIOCOUTQ, &outq) < 0) outq = 0;
    return pending() + (size_t)std::max(0, outq);
}

void RtspConn::on_packet(const Packet &p) {
    if (!playing_ || closed()) return;
    if (stream_->generation() != gen_) {  // encoder was reconfigured: client must re-DESCRIBE
        close();
        return;
    }
    size_t limit = std::max(kMinBacklog, (size_t)(stream_->kbps() * 1000 / 8 * 0.4f));
    size_t queued = backlog();
    if (!need_key_ && queued > limit) {
        // too slow: drop to the next keyframe, and ask for one now rather than waiting out the GOP
        // (at most every 3 s, a keyframe costs every viewer bitrate)
        need_key_ = true;
        if (stream_->request_idr && now_ms() - last_skip_idr_ > 3000) {
            last_skip_idr_ = now_ms();
            stream_->request_idr();
        }
    }
    if (need_key_) {
        if (!p.key || queued > limit / 4) return;
        need_key_ = false;
    }
    if (ts_base_us_ < 0) ts_base_us_ = p.pts_us;
    uint32_t ts = (uint32_t)((p.pts_us - ts_base_us_) * 9 / 100);  // 90 kHz
    auto nals = split_nals((const uint8_t *)p.data->data(), p.data->size(), p.codec);
    for (size_t i = 0; i < nals.size(); ++i) {
        const Nal &n = nals[i];
        bool last = i + 1 == nals.size();
        if (p.codec == Codec::H264 && (n.type == 9)) continue;  // access unit delimiter
        if (n.n <= kMtu) {
            send_rtp(nullptr, 0, n.p, n.n, last, ts);
            continue;
        }
        if (p.codec == Codec::H264) {
            // FU-A
            uint8_t ind = (n.p[0] & 0xe0) | 28, type = n.p[0] & 0x1f;
            size_t off = 1;
            while (off < n.n) {
                size_t chunk = std::min(kMtu - 2, n.n - off);
                uint8_t hdr[2] = {ind, (uint8_t)(type | (off == 1 ? 0x80 : 0) | (off + chunk == n.n ? 0x40 : 0))};
                send_rtp(hdr, 2, n.p + off, chunk, last && off + chunk == n.n, ts);
                off += chunk;
            }
        } else {
            // H.265 FU (type 49)
            uint8_t h0 = (n.p[0] & 0x81) | (49 << 1), h1 = n.p[1], type = (n.p[0] >> 1) & 0x3f;
            size_t off = 2;
            while (off < n.n) {
                size_t chunk = std::min(kMtu - 3, n.n - off);
                uint8_t hdr[3] = {h0, h1, (uint8_t)(type | (off == 2 ? 0x80 : 0) | (off + chunk == n.n ? 0x40 : 0))};
                send_rtp(hdr, 3, n.p + off, chunk, last && off + chunk == n.n, ts);
                off += chunk;
            }
        }
    }
    if (now_ms() - last_sr_ > 3000) {
        // the report pairs the wall clock with the RTP time of *now* (not of this picture's capture), so players
        // and NVRs that sync on it get the camera's real delay
        last_sr_ = now_ms();
        int64_t lag = now_us() - p.pts_us;  // capture -> now; only trusted when both clocks clearly agree
        send_rtcp_sr(ts + (uint32_t)(lag > 0 && lag < 2000000 ? lag * 9 / 100 : 0));
    }
}

bool start(int port) {
    g_port = port;
    return g_listener.listen_tcp(port, [](int fd, const std::string &peer) { return std::make_shared<RtspConn>(fd, peer); });
}

void stop() {
    g_listener.close();
    kick_all();
}

void configure(const std::string &mode) { g_auth = mode; }

int sessions() {
    int n = 0;
    for (auto *c : g_conns) n += c->playing();
    return n;
}

void kick_all() {
    std::vector<RtspConn *> v(g_conns.begin(), g_conns.end());
    for (auto *c : v) c->kick();
}

}  // namespace rtsp
