#include "mp4.h"

#include <unistd.h>

#include "core/util.h"

#include <cstring>

namespace {

// big-endian box builder
struct Buf {
    std::string s;
    void u8(uint32_t v) { s += (char)v; }
    void u16(uint32_t v) { u8(v >> 8), u8(v & 0xff); }
    void u24(uint32_t v) { u8(v >> 16), u16(v & 0xffff); }
    void u32(uint32_t v) { u16(v >> 16), u16(v & 0xffff); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)), u32((uint32_t)v); }
    void str(const char *t) { s += t; }
    void raw(const std::string &d) { s += d; }
    void zeros(int n) { s.append(n, '\0'); }
};

std::string box(const char *type, const std::string &payload) {
    Buf b;
    b.u32(8 + payload.size());
    b.str(type);
    b.raw(payload);
    return b.s;
}

std::string fullbox(const char *type, uint8_t ver, uint32_t flags, const std::string &payload) {
    Buf b;
    b.u8(ver);
    b.u24(flags);
    b.raw(payload);
    return box(type, b.s);
}

const uint32_t kTimescale = 90000;

std::string avcc(const Stream::Params &ps) {
    Buf b;
    b.u8(1);
    b.u8(ps.sps.size() > 1 ? (uint8_t)ps.sps[1] : 100);
    b.u8(ps.sps.size() > 2 ? (uint8_t)ps.sps[2] : 0);
    b.u8(ps.sps.size() > 3 ? (uint8_t)ps.sps[3] : 40);
    b.u8(0xff);  // 4-byte NAL lengths
    b.u8(0xe1);
    b.u16(ps.sps.size());
    b.raw(ps.sps);
    b.u8(1);
    b.u16(ps.pps.size());
    b.raw(ps.pps);
    return box("avcC", b.s);
}

std::string hvcc(const Stream::Params &ps) {
    Buf b;
    b.u8(1);
    // general_profile_space/tier/profile_idc, compat flags (4), constraint flags (6), level: copied from the SPS
    // profile_tier_level, which starts after the 2-byte NAL header and 1 byte of SPS fields
    std::string sps = nal_unescape(ps.sps);
    if (sps.size() >= 15) {
        b.raw(sps.substr(3, 12));
    } else {
        b.u8(1), b.u32(0x60000000), b.zeros(6), b.u8(120);
    }
    b.u16(0xf000);  // min_spatial_segmentation_idc
    b.u8(0xfc);     // parallelismType
    b.u8(0xfd);     // chroma 4:2:0
    b.u8(0xf8);     // luma bit depth 8
    b.u8(0xf8);     // chroma bit depth 8
    b.u16(0);       // avgFrameRate
    b.u8(0x0f);     // constFrameRate 0, numTemporalLayers 1, temporalIdNested 1, lengthSizeMinusOne 3
    b.u8(3);
    const std::pair<int, const std::string *> arr[3] = {{32, &ps.vps}, {33, &ps.sps}, {34, &ps.pps}};
    for (auto &a : arr) {
        b.u8(0x80 | a.first);
        b.u16(1);
        b.u16(a.second->size());
        b.raw(*a.second);
    }
    return box("hvcC", b.s);
}

}  // namespace

std::string mp4_init_segment(Codec c, int w, int h, const Stream::Params &ps) {
    Buf ftyp;
    ftyp.str("isom");
    ftyp.u32(0x200);
    ftyp.str("isomiso6mp41");
    ftyp.str(c == Codec::H265 ? "hvc1" : "avc1");

    Buf mvhd;
    mvhd.u32(0), mvhd.u32(0), mvhd.u32(kTimescale), mvhd.u32(0);
    mvhd.u32(0x00010000), mvhd.u16(0x0100), mvhd.zeros(10);
    const uint32_t matrix[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
    for (uint32_t m : matrix) mvhd.u32(m);
    mvhd.zeros(24);
    mvhd.u32(2);  // next track id

    Buf tkhd;
    tkhd.u32(0), tkhd.u32(0), tkhd.u32(1), tkhd.u32(0), tkhd.u32(0);
    tkhd.zeros(8), tkhd.u16(0), tkhd.u16(0), tkhd.u16(0), tkhd.u16(0);
    for (uint32_t m : matrix) tkhd.u32(m);
    tkhd.u32(w << 16), tkhd.u32(h << 16);

    Buf mdhd;
    mdhd.u32(0), mdhd.u32(0), mdhd.u32(kTimescale), mdhd.u32(0), mdhd.u16(0x55c4), mdhd.u16(0);
    Buf hdlr;
    hdlr.u32(0), hdlr.str("vide"), hdlr.zeros(12), hdlr.str("PicoCam Video"), hdlr.u8(0);
    Buf vmhd;
    vmhd.zeros(8);
    Buf dref;
    dref.u32(1);
    dref.raw(fullbox("url ", 0, 1, ""));

    Buf vse;  // visual sample entry
    vse.zeros(6), vse.u16(1), vse.zeros(16), vse.u16(w), vse.u16(h), vse.u32(0x00480000), vse.u32(0x00480000), vse.u32(0);
    vse.u16(1), vse.zeros(32), vse.u16(0x18), vse.u16(0xffff);
    vse.raw(c == Codec::H265 ? hvcc(ps) : avcc(ps));
    Buf stsd;
    stsd.u32(1);
    stsd.raw(box(c == Codec::H265 ? "hvc1" : "avc1", vse.s));
    Buf empty4;
    empty4.u32(0);
    Buf stsz;
    stsz.u32(0), stsz.u32(0);
    std::string stbl = box("stbl", fullbox("stsd", 0, 0, stsd.s) + fullbox("stts", 0, 0, empty4.s) +
                                       fullbox("stsc", 0, 0, empty4.s) + fullbox("stsz", 0, 0, stsz.s) +
                                       fullbox("stco", 0, 0, empty4.s));
    std::string minf = box("minf", fullbox("vmhd", 0, 1, vmhd.s) + box("dinf", fullbox("dref", 0, 0, dref.s)) + stbl);
    std::string mdia = box("mdia", fullbox("mdhd", 0, 0, mdhd.s) + fullbox("hdlr", 0, 0, hdlr.s) + minf);
    std::string trak = box("trak", fullbox("tkhd", 0, 3, tkhd.s) + mdia);
    Buf trex;
    trex.u32(1), trex.u32(1), trex.u32(0), trex.u32(0), trex.u32(0x00010000);
    std::string mvex = box("mvex", fullbox("trex", 0, 0, trex.s));
    std::string head = box("ftyp", ftyp.s) + box("moov", fullbox("mvhd", 0, 0, mvhd.s) + trak + mvex);
    return head;
}

bool Mp4Writer::open(const std::string &path, Codec c, int w, int h, const Stream::Params &ps) {
    close();
    fp_ = fopen(path.c_str(), "wb");
    if (!fp_) return false;
    codec_ = c;
    seq_ = 1;
    first_pts_ = -1;
    base_dts_ = 0;
    bytes_ = 0;

    std::string head = mp4_init_segment(c, w, h, ps);
    if (fwrite(head.data(), 1, head.size(), fp_) != head.size()) {
        close();
        return false;
    }
    bytes_ = head.size();
    return true;
}

std::string mp4_sample_data(const Packet &p) {
    std::string out;
    for (auto &n : split_nals((const uint8_t *)p.data->data(), p.data->size(), p.codec)) {
        // parameter sets live in the sample entry; access unit delimiters are not needed
        if (p.codec == Codec::H264 && (n.type == 7 || n.type == 8 || n.type == 9)) continue;
        if (p.codec == Codec::H265 && (n.type == 32 || n.type == 33 || n.type == 34 || n.type == 35)) continue;
        uint32_t len = (uint32_t)n.n;
        char l[4] = {(char)(len >> 24), (char)(len >> 16), (char)(len >> 8), (char)len};
        out.append(l, 4);
        out.append((const char *)n.p, n.n);
    }
    return out;
}

std::string mp4_fragment(uint32_t seq, int64_t base_dts_us, const std::vector<Mp4Sample> &samples) {
    size_t n = samples.size();
    Buf mfhd;
    mfhd.u32(seq);
    Buf tfhd;
    tfhd.u32(1);
    Buf tfdt;
    tfdt.u64((uint64_t)(base_dts_us * kTimescale / 1000000));
    Buf trun;
    trun.u32(n);
    size_t trun_off_pos = trun.s.size();
    trun.u32(0);  // data offset, patched below
    size_t mdat_size = 0;
    for (auto &smp : samples) {
        trun.u32((uint32_t)((int64_t)smp.dur_us * kTimescale / 1000000));
        trun.u32(smp.data.size());
        // sync sample vs. non-sync, depends-on-others
        trun.u32(smp.key ? 0x02000000 : 0x01010000);
        mdat_size += smp.data.size();
    }
    std::string traf_wo_trun = fullbox("tfhd", 0, 0x020000, tfhd.s) + fullbox("tfdt", 1, 0, tfdt.s);
    size_t trun_box = 12 + trun.s.size();
    size_t moof_size = 8 + (8 + mfhd.s.size() + 4) + 8 + traf_wo_trun.size() + trun_box;
    uint32_t data_off = (uint32_t)(moof_size + 8);
    for (int i = 0; i < 4; ++i) trun.s[trun_off_pos + i] = (char)(data_off >> (24 - 8 * i));
    std::string out = box("moof", fullbox("mfhd", 0, 0, mfhd.s) + box("traf", traf_wo_trun + fullbox("trun", 0, 0x000701, trun.s)));
    Buf mh;
    mh.u32(8 + mdat_size);
    mh.str("mdat");
    out += mh.s;
    out.reserve(out.size() + mdat_size);
    for (auto &smp : samples) out += smp.data;
    return out;
}

bool Mp4Writer::write(const Packet &p) {
    if (!fp_) return false;
    if (first_pts_ < 0) {
        if (!p.key) return true;  // start on a keyframe
        first_pts_ = p.pts_us;
    }
    if (p.key && !pending_.empty() && !flush_fragment()) return false;
    Mp4Sample s;
    s.pts = p.pts_us;
    s.key = p.key;
    s.data = mp4_sample_data(p);
    last_pts_ = p.pts_us;
    pending_.push_back(std::move(s));
    // don't let a long GOP hold too much in RAM
    if (pending_.size() >= 120) return flush_fragment();
    return true;
}

bool Mp4Writer::flush_fragment() {
    if (pending_.empty()) return true;
    size_t n = pending_.size();
    int64_t avg = n > 1 ? (pending_[n - 1].pts - pending_[0].pts) / (int64_t)(n - 1) : 40000;
    for (size_t i = 0; i < n; ++i) {
        int64_t next = i + 1 < n ? pending_[i + 1].pts : pending_[i].pts + avg;
        pending_[i].dur_us = (uint32_t)std::max<int64_t>(1000, next - pending_[i].pts);
    }
    std::string frag = mp4_fragment(seq_++, base_dts_, pending_);
    bool ok = fwrite(frag.data(), 1, frag.size(), fp_) == frag.size();
    fflush(fp_);
    // push the data to the card every few seconds: what is only in the page cache is lost when the power goes
    static thread_local int64_t last_sync = 0;
    int64_t t = now_ms();
    if (t - last_sync > 5000) {
        fdatasync(fileno(fp_));
        last_sync = t;
    }
    bytes_ += frag.size();
    for (auto &smp : pending_) base_dts_ += smp.dur_us;
    pending_.clear();
    return ok;
}

void Mp4Writer::close() {
    if (!fp_) return;
    flush_fragment();
    fclose(fp_);
    fp_ = nullptr;
    pending_.clear();
}
