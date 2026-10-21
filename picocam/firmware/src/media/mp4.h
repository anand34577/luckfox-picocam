#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "stream.h"

struct Mp4Sample {
    std::string data;  // length-prefixed NAL units
    int64_t pts = 0;   // microseconds
    uint32_t dur_us = 0;
    bool key = false;
};
// Building blocks (also used to stream live fMP4 to browsers through Media Source Extensions)
std::string mp4_init_segment(Codec c, int w, int h, const Stream::Params &ps);
std::string mp4_fragment(uint32_t seq, int64_t base_dts_us, const std::vector<Mp4Sample> &samples);
// Annex-B access unit -> length-prefixed sample (parameter sets and AUDs dropped)
std::string mp4_sample_data(const Packet &p);

// Fragmented MP4 writer (ISO BMFF): moov up front, then one moof+mdat per GOP. A file cut short by a power
// loss stays playable up to the last complete fragment. Plays in browsers, VLC, ffmpeg.
class Mp4Writer {
public:
    ~Mp4Writer() { close(); }
    bool open(const std::string &path, Codec c, int w, int h, const Stream::Params &ps);
    bool write(const Packet &p);  // Annex-B access unit
    void close();
    bool is_open() const { return fp_ != nullptr; }
    int64_t bytes() const { return bytes_; }
    int64_t duration_ms() const { return (last_pts_ - first_pts_) / 1000; }

private:
    bool flush_fragment();
    FILE *fp_ = nullptr;
    Codec codec_ = Codec::H264;
    std::vector<Mp4Sample> pending_;
    uint32_t seq_ = 1;
    int64_t first_pts_ = -1, last_pts_ = 0, base_dts_ = 0;  // microseconds
    int64_t bytes_ = 0;
};
