#include "stream.h"

#include "core/util.h"

Stream g_main_stream(0), g_sub_stream(1);

std::vector<Nal> split_nals(const uint8_t *d, size_t n, Codec c) {
    std::vector<Nal> out;
    size_t i = 0, start = std::string::npos;
    auto push = [&](size_t s, size_t e) {
        while (e > s && d[e - 1] == 0) --e;  // trailing zeros belong to the next start code
        if (e <= s) return;
        int type = c == Codec::H264 ? (d[s] & 0x1f) : ((d[s] >> 1) & 0x3f);
        out.push_back({d + s, e - s, type});
    };
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != std::string::npos) push(start, i);
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != std::string::npos) push(start, n);
    return out;
}

std::string nal_unescape(const std::string &nal) {
    std::string o;
    o.reserve(nal.size());
    int zeros = 0;
    for (char ch : nal) {
        uint8_t c = (uint8_t)ch;
        if (zeros >= 2 && c == 3) {
            zeros = 0;
            continue;
        }
        zeros = c == 0 ? zeros + 1 : 0;
        o += ch;
    }
    return o;
}

int Stream::subscribe(Sink s) {
    std::lock_guard<std::mutex> lk(mu_);
    int id = next_id_++;
    sinks_[id] = std::move(s);
    return id;
}

void Stream::unsubscribe(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    sinks_.erase(id);
}

size_t Stream::subscribers() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sinks_.size();
}

void Stream::reset(Codec c, int w, int h, int fps) {
    std::lock_guard<std::mutex> lk(mu_);
    codec_ = c;
    w_ = w;
    h_ = h;
    fps_ = fps;
    params_ = Params();
    last_key_ = Packet();
    ++gen_;
    last_pub_ = now_ms();
}

Stream::Params Stream::params() const {
    std::lock_guard<std::mutex> lk(mu_);
    return params_;
}

Packet Stream::last_key() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_key_;
}

int64_t Stream::idle_ms() const {
    int64_t t = last_pub_;
    return t ? now_ms() - t : 0;
}

void Stream::publish(const Packet &p) {
    last_pub_ = now_ms();
    if (p.key) {
        Params ps;
        for (auto &nal : split_nals((const uint8_t *)p.data->data(), p.data->size(), p.codec)) {
            std::string raw((const char *)nal.p, nal.n);
            if (p.codec == Codec::H264) {
                if (nal.type == 7) ps.sps = raw;
                else if (nal.type == 8) ps.pps = raw;
            } else {
                if (nal.type == 32) ps.vps = raw;
                else if (nal.type == 33) ps.sps = raw;
                else if (nal.type == 34) ps.pps = raw;
            }
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (ps.valid()) params_ = ps;
        last_key_ = p;
    }
    // rate statistics over ~2 s windows
    int64_t t = now_ms();
    if (!stat_t0_) stat_t0_ = t;
    stat_bytes_ += p.data->size();
    ++stat_frames_;
    if (t - stat_t0_ >= 2000) {
        kbps_ = stat_bytes_ * 8.f / (t - stat_t0_);
        mfps_ = stat_frames_ * 1000.f / (t - stat_t0_);
        stat_t0_ = t;
        stat_bytes_ = stat_frames_ = 0;
    }
    std::lock_guard<std::mutex> lk(mu_);
    for (auto &s : sinks_) s.second(p);
}
