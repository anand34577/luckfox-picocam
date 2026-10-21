#include "live.h"

#include <atomic>
#include <set>

#include "core/log.h"
#include "core/util.h"
#include "media/mp4.h"
#include "media/stream.h"
#include "net/http.h"

namespace live {

static const size_t kMaxBacklog = 1536 * 1024;

static std::string codec_string(Codec c, const Stream::Params &ps) {
    if (c == Codec::H264) {
        if (ps.sps.size() < 4) return "avc1.640028";
        return "avc1." + hex((const uint8_t *)ps.sps.data() + 1, 3);
    }
    // H.265 SPS: 2-byte NAL header, 1 byte (vps id / sub layers), then profile_tier_level
    int profile = 1, level = 120;
    bool high_tier = false;
    std::string sps = nal_unescape(ps.sps);
    if (sps.size() > 15) {
        const uint8_t *s = (const uint8_t *)sps.data();
        profile = s[3] & 0x1f;
        high_tier = s[3] & 0x20;
        level = s[3 + 1 + 4 + 6];
    }
    return std::string("hvc1.") + std::to_string(profile) + (profile == 2 ? ".4." : ".6.") + (high_tier ? "H" : "L") +
           std::to_string(level) + ".B0";
}

class LiveSession : public WsSession {
public:
    LiveSession(int idx, bool mp4) : stream_(stream_by_index(idx)), mp4_(mp4) {}
    ~LiveSession() override {
        if (sub_) stream_->unsubscribe(sub_);
    }
    void on_open() override {
        gen_ = stream_->generation();
        std::weak_ptr<WsSession> self = shared_from_this();
        sub_ = stream_->subscribe([self](const Packet &p) {
            g_loop.post([self, p] {
                if (auto s = self.lock()) static_cast<LiveSession *>(s.get())->on_packet(p);
            });
        });
        if (stream_->request_idr) stream_->request_idr();
    }
    void on_close() override {
        if (sub_) stream_->unsubscribe(sub_);
        sub_ = 0;
    }
    void on_text(const std::string &msg) override {
        // the player asks for a keyframe after a decoder error
        if (msg.find("\"keyframe\"") != std::string::npos) {
            need_key_ = true;
            if (stream_->request_idr) stream_->request_idr();
        }
    }

private:
    void send_init() {
        auto ps = stream_->params();
        json j = {{"type", "init"},
                  {"codec", codec_string(stream_->codec(), ps)},
                  {"format", codec_name(stream_->codec())},
                  {"width", stream_->width()},
                  {"height", stream_->height()},
                  {"fps", stream_->fps()},
                  {"transport", mp4_ ? "fmp4" : "annexb"}};
        send_text(j.dump());
        if (mp4_) {
            // MSE needs the init segment (ftyp+moov) before any fragment
            std::string hdr(9, '\0');
            hdr[0] = 2;
            send_binary(hdr, std::make_shared<std::string>(mp4_init_segment(stream_->codec(), stream_->width(), stream_->height(), ps)));
            seq_ = 1;
            dts_ = 0;
        }
        inited_ = true;
    }
    void on_packet(const Packet &p) {
        if (!alive()) return;
        if (stream_->generation() != gen_) {  // resolution/codec changed: re-init the decoder
            gen_ = stream_->generation();
            inited_ = false;
            need_key_ = true;
        }
        if (pending() > kMaxBacklog) need_key_ = true;
        if (need_key_) {
            if (!p.key || pending() > kMaxBacklog / 4) return;
            need_key_ = false;
        }
        if (!inited_) send_init();
        std::string hdr(9, '\0');
        hdr[0] = p.key ? 1 : 0;
        uint64_t pts = (uint64_t)p.pts_us;
        for (int i = 0; i < 8; ++i) hdr[1 + i] = (char)(pts >> (56 - 8 * i));
        if (!mp4_) {
            send_binary(hdr, p.data);
            return;
        }
        // one fragment per frame keeps MSE latency to roughly one frame
        Mp4Sample smp;
        smp.data = mp4_sample_data(p);
        smp.key = p.key;
        smp.dur_us = (uint32_t)(1000000 / std::max(1, stream_->fps()));
        auto frag = std::make_shared<std::string>(mp4_fragment(seq_++, dts_, {smp}));
        dts_ += smp.dur_us;
        send_binary(hdr, frag);
    }

    Stream *stream_;
    bool mp4_;
    uint32_t seq_ = 1;
    int64_t dts_ = 0;
    int sub_ = 0;
    int64_t gen_ = 0;
    bool need_key_ = true, inited_ = false;
};

// ---------------------------------------------------------------- JSON bus

class BusSession : public WsSession {
public:
    void on_open() override;
    void on_close() override;
    void on_text(const std::string &msg) override {
        try {
            json j = json::parse(msg);
            if (j.contains("subscribe")) {
                topics_.clear();
                for (auto &t : j["subscribe"]) topics_.insert(t.get<std::string>());
            }
        } catch (...) {
        }
    }
    bool wants(const std::string &t) const { return topics_.count("*") || topics_.count(t); }
    std::set<std::string> topics_{"*"};
};

static std::set<BusSession *> g_bus;
static std::atomic<int> g_bus_n{0};  // lets producers skip building JSON nobody will read

void BusSession::on_open() {
    g_bus.insert(this);
    g_bus_n = (int)g_bus.size();
}
void BusSession::on_close() {
    g_bus.erase(this);
    g_bus_n = (int)g_bus.size();
}

void publish(const std::string &topic, const json &msg) {
    if (g_bus_n == 0) return;
    json m = msg;
    m["topic"] = topic;
    auto text = std::make_shared<std::string>(m.dump(-1, ' ', false, json::error_handler_t::replace));
    g_loop.post([topic, text] {
        for (auto *s : g_bus)
            if (s->wants(topic) && s->alive() && s->pending() < 512 * 1024) s->send_text(*text);
    });
}

int viewers(){ return (int)g_main_stream.subscribers() + (int)g_sub_stream.subscribers(); }

void register_routes() {
    g_http.websocket("/ws/live", auth::VIEWER, [](const HttpRequest &req) -> std::shared_ptr<WsSession> {
        std::string s = req.param("stream", "sub");
        return std::make_shared<LiveSession>(s == "main" || s == "0" ? 0 : 1, req.param("fmt") == "mp4");
    });
    g_http.websocket("/ws/events", auth::VIEWER,
                     [](const HttpRequest &) -> std::shared_ptr<WsSession> { return std::make_shared<BusSession>(); });
}

}  // namespace live
