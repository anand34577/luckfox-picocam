#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class Codec { H264, H265 };
inline const char *codec_name(Codec c) { return c == Codec::H265 ? "h265" : "h264"; }

// One encoded access unit (Annex-B, start codes included).
struct Packet {
    std::shared_ptr<const std::string> data;
    int64_t pts_us = 0;  // capture time, monotonic microseconds
    bool key = false;
    Codec codec = Codec::H264;
};

// NAL unit view into an Annex-B buffer.
struct Nal {
    const uint8_t *p;
    size_t n;
    int type;  // H.264: nal_unit_type (5 bits); H.265: nal_unit_type (6 bits)
};
std::vector<Nal> split_nals(const uint8_t *d, size_t n, Codec c);
// NAL payload with emulation-prevention bytes (00 00 03) removed, for parsing SPS fields
std::string nal_unescape(const std::string &nal);

// Fan-out of one encoder's output. publish() runs on the encoder thread; subscriber callbacks must only
// queue the packet and return (network/recording threads do the actual work).
class Stream {
public:
    explicit Stream(int index) : index_(index) {}
    using Sink = std::function<void(const Packet &)>;

    int subscribe(Sink s);
    void unsubscribe(int id);
    void publish(const Packet &p);
    void reset(Codec c, int w, int h, int fps);  // new encoder configuration (clears parameter sets)

    // parameter sets from the latest keyframe (raw NAL payloads without start codes)
    struct Params {
        std::string vps, sps, pps;
        bool valid() const { return !sps.empty() && !pps.empty(); }
    };
    Params params() const;
    Packet last_key() const;  // most recent keyframe (lets new viewers start instantly)

    int index() const { return index_; }
    Codec codec() const { return codec_; }
    int width() const { return w_; }
    int height() const { return h_; }
    int fps() const { return fps_; }
    int64_t generation() const { return gen_; }  // bumps on every reset(): sessions must restart
    float kbps() const { return kbps_; }
    float measured_fps() const { return mfps_; }
    size_t subscribers() const;
    // time since the encoder last produced a packet (watchdog liveness)
    int64_t idle_ms() const;

    std::function<void()> request_idr;  // set by the video pipeline

private:
    int index_;
    mutable std::mutex mu_;
    std::map<int, Sink> sinks_;
    int next_id_ = 1;
    Params params_;
    Packet last_key_;
    std::atomic<Codec> codec_{Codec::H264};
    std::atomic<int> w_{0}, h_{0}, fps_{0};
    std::atomic<int64_t> gen_{0};
    std::atomic<int64_t> last_pub_{0};
    // rate stats
    int64_t stat_t0_ = 0, stat_bytes_ = 0, stat_frames_ = 0;
    std::atomic<float> kbps_{0}, mfps_{0};
};

extern Stream g_main_stream, g_sub_stream;
inline Stream *stream_by_index(int i) { return i == 0 ? &g_main_stream : i == 1 ? &g_sub_stream : nullptr; }
