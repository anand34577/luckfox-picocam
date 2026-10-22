#include "video.h"

#include <sys/prctl.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "core/board.h"
#include "core/log.h"
#include "core/util.h"
#include "isp.h"
#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "rk_mpi_mb.h"
#include "rk_mpi_mmz.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "stream.h"

namespace video {

static const char *TAG = "video";
static const int kPipe = 0, kDev = 0;
static const int kChMain = 0, kChSub = 1, kChJpeg = 2;
// region handles
static const int kOsdBase = 0;    // 0..3: main time, main name, sub time, sub name
static const int kMaskBase = 10;  // 10..17: privacy masks

struct StreamCfg {
    bool enabled = true;
    Codec codec = Codec::H264;
    int w = 0, h = 0, fps = 25, gop = 50, bitrate = 2048, max_bitrate = 3072, profile = 100;
    std::string rc = "cbr", quality = "high";
    bool smart_p = false;
};

static std::mutex g_mu;  // start/stop/restart
static std::atomic<int64_t> g_trans_since{0};  // a start/stop is in progress since (monotonic ms)
static std::atomic<int64_t> g_fail_since{0};   // the last start failed: no video since (monotonic ms)
static std::atomic<bool> g_run{false};
static bool g_sys_init = false;
static StreamCfg g_main, g_sub;
static int g_sensor_fps = 25;
static std::thread g_th_venc[2], g_th_sub, g_th_osd;
static std::mutex g_jpeg_mu;
static int g_jpeg_quality = 80;

static std::atomic<float> g_cap_isp_ms{0}, g_cap_hooks_ms{0}, g_cap_total_ms{0};  // capture thread, per sub frame
static std::mutex g_hook_mu;
static std::map<int, FrameHook> g_hooks;
static int g_hook_next = 1;

static std::mutex g_osd_mu;
static json g_osd_cfg;
static int g_mask_count = 0;

int add_frame_hook(FrameHook h) {
    std::lock_guard<std::mutex> lk(g_hook_mu);
    g_hooks[g_hook_next] = std::move(h);
    return g_hook_next++;
}

void remove_frame_hook(int id) {
    std::lock_guard<std::mutex> lk(g_hook_mu);
    g_hooks.erase(id);
}

// ---------------------------------------------------------------- config

// a stored/default size larger than this camera module can deliver (another sensor, or a config restored from one) is scaled
// down to fit instead of leaving the board without video
static void fit_to_sensor(StreamCfg &s, const char *stream) {
    int mw = board::max_width(stream), mh = board::max_height(stream);
    if (s.w <= mw && s.h <= mh) return;
    double k = std::min((double)mw / s.w, (double)mh / s.h);
    LOGW(TAG, "%s stream %dx%d exceeds the sensor (%dx%d): scaled down", stream, s.w, s.h, mw, mh);
    s.w = std::max(320, (int)(s.w * k) & ~15);
    s.h = std::max(180, (int)(s.h * k) & ~1);
}

static StreamCfg parse_stream(const json &j, int sensor_fps) {
    StreamCfg s;
    s.enabled = jget<bool>(j, "enabled", true);
    s.codec = jget<std::string>(j, "codec", "h264") == "h265" ? Codec::H265 : Codec::H264;
    s.w = jget<int>(j, "width", 1920) & ~15;
    s.h = jget<int>(j, "height", 1080) & ~1;
    s.fps = std::max(1, std::min(sensor_fps, jget<int>(j, "fps", sensor_fps)));
    s.gop = std::max(1, std::min(600, jget<int>(j, "gop", s.fps * 2)));
    s.bitrate = std::max(64, std::min(20000, jget<int>(j, "bitrate", 2048)));
    s.max_bitrate = std::max(s.bitrate, std::min(30000, jget<int>(j, "max_bitrate", s.bitrate * 3 / 2)));
    s.rc = jget<std::string>(j, "rc", "cbr");
    s.quality = jget<std::string>(j, "quality", "high");
    s.smart_p = jget<bool>(j, "smart_p", false);
    std::string prof = jget<std::string>(j, "profile", "high");
    s.profile = prof == "baseline" ? 66 : prof == "main" ? 77 : 100;
    return s;
}

std::string validate(json &v) {
    int sfps = jget<int>(v, "sensor_fps", 25);
    if (sfps < 5 || sfps > 30) return "sensor_fps must be 5..30";
    for (const char *k : {"main", "sub"}) {
        json &s = v[k];
        int w = jget<int>(s, "width", 0), h = jget<int>(s, "height", 0);
        int maxw = board::max_width(k), maxh = board::max_height(k);
        if (w < 320 || h < 180 || w > maxw || h > maxh)
            return std::string(k) + ": resolution out of range (this camera module delivers up to " + std::to_string(maxw) + "x" +
                   std::to_string(maxh) + ")";
        if (w % 16) s["width"] = w & ~15;
        if (h % 2) s["height"] = h & ~1;
        std::string c = jget<std::string>(s, "codec", "h264");
        if (c != "h264" && c != "h265") return std::string(k) + ": codec must be h264 or h265";
        std::string rc = jget<std::string>(s, "rc", "cbr");
        if (rc != "cbr" && rc != "vbr" && rc != "avbr") return std::string(k) + ": rc must be cbr, vbr or avbr";
        int fps = jget<int>(s, "fps", sfps);
        if (fps < 1 || fps > sfps) return std::string(k) + ": fps must be 1..sensor fps";
        int br = jget<int>(s, "bitrate", 0);
        if (br < 64 || br > 20000) return std::string(k) + ": bitrate must be 64..20000 kbps";
    }
    if (!jget<bool>(v["main"], "enabled", true) && !jget<bool>(v["sub"], "enabled", true))
        return "at least one stream must stay enabled";
    return "";
}

// ---------------------------------------------------------------- VI / VENC setup

static bool vi_dev_init() {
    VI_DEV_ATTR_S dev;
    memset(&dev, 0, sizeof(dev));
    if (RK_MPI_VI_GetDevAttr(kDev, &dev) == RK_ERR_VI_NOT_CONFIG && RK_MPI_VI_SetDevAttr(kDev, &dev) != RK_SUCCESS) {
        LOGE(TAG, "VI SetDevAttr failed");
        return false;
    }
    if (RK_MPI_VI_GetDevIsEnable(kDev) != RK_SUCCESS) {
        if (RK_MPI_VI_EnableDev(kDev) != RK_SUCCESS) {
            LOGE(TAG, "VI EnableDev failed");
            return false;
        }
        VI_DEV_BIND_PIPE_S bind;
        memset(&bind, 0, sizeof(bind));
        bind.u32Num = 1;
        bind.PipeId[0] = kPipe;
        if (RK_MPI_VI_SetDevBindPipe(kDev, &bind) != RK_SUCCESS) {
            LOGE(TAG, "VI SetDevBindPipe failed");
            return false;
        }
    }
    return true;
}

static std::vector<MB_BLK> g_reserve;

void sys_init() {
    if (g_sys_init) return;
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        LOGE(TAG, "MPI init failed");
        return;
    }
    if (vi_dev_init()) g_sys_init = true;
}

bool cma_reserve(const std::vector<size_t> &blocks) {
    cma_release();
    std::vector<size_t> v(blocks);
    std::sort(v.rbegin(), v.rend());
    size_t done = 0;
    for (size_t sz : v) {
        MB_BLK b = nullptr;
        if (RK_MPI_MMZ_Alloc(&b, (RK_U32)sz, RK_MMZ_ALLOC_TYPE_CMA) != RK_SUCCESS || !b) {
            LOGW(TAG, "reserved only %zu KB of media memory for AI", done / 1024);
            return false;
        }
        g_reserve.push_back(b);
        done += sz;
    }
    return true;
}

void cma_release() {
    for (auto b : g_reserve) RK_MPI_MMZ_Free(b);
    g_reserve.clear();
}

static bool vi_chn_init(int ch, const StreamCfg &s, int bufs, int depth, bool wrap) {
    VI_CHN_ATTR_S a;
    memset(&a, 0, sizeof(a));
    a.stIspOpt.u32BufCount = bufs;
    a.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    a.stIspOpt.stMaxSize.u32Width = s.w;
    a.stIspOpt.stMaxSize.u32Height = s.h;
    a.stSize.u32Width = s.w;
    a.stSize.u32Height = s.h;
    a.enPixelFormat = RK_FMT_YUV420SP;
    a.enCompressMode = COMPRESS_MODE_NONE;
    a.u32Depth = depth;
    a.stFrameRate.s32SrcFrameRate = g_sensor_fps;
    a.stFrameRate.s32DstFrameRate = s.fps;
    if (RK_MPI_VI_SetChnAttr(kPipe, ch, &a) != RK_SUCCESS) {
        LOGE(TAG, "VI ch%d attr failed", ch);
        return false;
    }
    if (wrap) {
        // the encoder reads lines as the ISP writes them: a quarter-frame buffer instead of 2-3 full frames
        VI_CHN_BUF_WRAP_S w;
        memset(&w, 0, sizeof(w));
        w.bEnable = RK_TRUE;
        w.u32BufLine = std::max(128, s.h / 6);
        w.u32WrapBufferSize = w.u32BufLine * s.w * 3 / 2;
        RK_MPI_VI_SetChnWrapBufAttr(kPipe, ch, &w);
    }
    if (RK_MPI_VI_EnableChn(kPipe, ch) != RK_SUCCESS) {
        LOGE(TAG, "VI ch%d enable failed", ch);
        return false;
    }
    return true;
}

static void fill_rc(VENC_CHN_ATTR_S &a, const StreamCfg &s) {
    // CBR/VBR/AVBR share the same leading layout in the rc union
    VENC_H264_VBR_S r;
    memset(&r, 0, sizeof(r));
    r.u32Gop = s.gop;
    r.u32SrcFrameRateNum = s.fps;
    r.u32SrcFrameRateDen = 1;
    r.fr32DstFrameRateNum = s.fps;
    r.fr32DstFrameRateDen = 1;
    r.u32BitRate = s.bitrate;
    r.u32MaxBitRate = s.max_bitrate;
    r.u32MinBitRate = std::max(32, s.bitrate / 4);
    r.u32StatTime = 3;
    bool h265 = s.codec == Codec::H265;
    if (s.rc == "vbr") {
        a.stRcAttr.enRcMode = h265 ? VENC_RC_MODE_H265VBR : VENC_RC_MODE_H264VBR;
        memcpy(&a.stRcAttr.stH264Vbr, &r, sizeof(r));
    } else if (s.rc == "avbr") {
        a.stRcAttr.enRcMode = h265 ? VENC_RC_MODE_H265AVBR : VENC_RC_MODE_H264AVBR;
        memcpy(&a.stRcAttr.stH264Avbr, &r, sizeof(r));
    } else {
        a.stRcAttr.enRcMode = h265 ? VENC_RC_MODE_H265CBR : VENC_RC_MODE_H264CBR;
        VENC_H264_CBR_S c;
        memset(&c, 0, sizeof(c));
        c.u32Gop = s.gop;
        c.u32SrcFrameRateNum = s.fps;
        c.u32SrcFrameRateDen = 1;
        c.fr32DstFrameRateNum = s.fps;
        c.fr32DstFrameRateDen = 1;
        c.u32BitRate = s.bitrate;
        c.u32StatTime = 3;
        memcpy(&a.stRcAttr.stH264Cbr, &c, sizeof(c));
    }
}

static bool venc_init(int ch, const StreamCfg &s, bool wrap) {
    VENC_CHN_ATTR_S a;
    memset(&a, 0, sizeof(a));
    a.stVencAttr.enType = s.codec == Codec::H265 ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;
    a.stVencAttr.u32Profile = s.codec == Codec::H265 ? 0 : s.profile;
    a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    a.stVencAttr.u32MaxPicWidth = s.w;
    a.stVencAttr.u32MaxPicHeight = s.h;
    a.stVencAttr.u32PicWidth = s.w;
    a.stVencAttr.u32PicHeight = s.h;
    a.stVencAttr.u32VirWidth = s.w;
    a.stVencAttr.u32VirHeight = s.h;
    a.stVencAttr.u32StreamBufCnt = ch == kChMain ? 4 : 3;
    // output ring buffer: 1/6 frame (~500 KB at 3 MP) holds any I-frame at these bitrates; the SDK's half frame
    // wastes media memory the NPU needs
    a.stVencAttr.u32BufSize = ch == kChMain ? s.w * s.h / 6 : s.w * s.h / 3;
    fill_rc(a, s);
    if (s.smart_p) {
        a.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
        a.stGopAttr.s32VirIdrLen = s.fps;
        a.stGopAttr.u32MaxLtrCount = 1;
    } else {
        a.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    }
    if (RK_MPI_VENC_CreateChn(ch, &a) != RK_SUCCESS) {
        LOGE(TAG, "VENC ch%d create failed (out of media memory? lower the resolution)", ch);
        RK_MPI_VENC_DestroyChn(ch);
        return false;
    }
    // quality: minimum QP caps how much bitrate a static scene may soak up
    VENC_RC_PARAM_S rp;
    if (RK_MPI_VENC_GetRcParam(ch, &rp) == RK_SUCCESS) {
        // the quality preset moves all three floors (slice, I-frame, P-frame); with a fixed frame floor the preset
        // would only change how low a *slice* may go and have little effect on a static scene
        int q = s.quality == "highest" ? 0 : s.quality == "high" ? 1 : s.quality == "medium" ? 2 : 3;
        static const int kMin[4] = {10, 20, 26, 32}, kMinI[4] = {18, 24, 28, 32}, kMinP[4] = {20, 26, 30, 34};
        int minqp = kMin[q], miniqp = kMinI[q], minpqp = kMinP[q];
        if (s.codec == Codec::H265) {
            rp.stParamH265.u32MinQp = minqp;
            rp.stParamH265.u32FrmMinIQp = miniqp;
            rp.stParamH265.u32FrmMinQp = minpqp;
            rp.stParamH265.u32FrmMaxIQp = 51;
            rp.stParamH265.u32FrmMaxQp = 51;
        } else {
            rp.stParamH264.u32MinQp = minqp;
            rp.stParamH264.u32FrmMinIQp = miniqp;
            rp.stParamH264.u32FrmMinQp = minpqp;
            rp.stParamH264.u32FrmMaxIQp = 51;
            rp.stParamH264.u32FrmMaxQp = 51;
        }
        RK_MPI_VENC_SetRcParam(ch, &rp);
    }
    if (wrap) {
        VENC_CHN_BUF_WRAP_S w;
        memset(&w, 0, sizeof(w));
        w.bEnable = RK_TRUE;
        w.u32BufLine = std::max(128, s.h / 6);  // must match the VI wrap line count
        RK_MPI_VENC_SetChnBufWrapAttr(ch, &w);
    }
    VENC_CHN_REF_BUF_SHARE_S share;
    memset(&share, 0, sizeof(share));
    share.bEnable = RK_TRUE;
    RK_MPI_VENC_SetChnRefBufShareAttr(ch, &share);
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    return RK_MPI_VENC_StartRecvFrame(ch, &recv) == RK_SUCCESS;
}

static bool jpeg_init(const StreamCfg &main) {
    VENC_CHN_ATTR_S a;
    memset(&a, 0, sizeof(a));
    a.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    a.stVencAttr.u32MaxPicWidth = main.w;
    a.stVencAttr.u32MaxPicHeight = main.h;
    a.stVencAttr.u32PicWidth = main.w;
    a.stVencAttr.u32PicHeight = main.h;
    a.stVencAttr.u32VirWidth = main.w;
    a.stVencAttr.u32VirHeight = main.h;
    a.stVencAttr.u32StreamBufCnt = 1;
    a.stVencAttr.u32BufSize = main.w * main.h / 6;
    if (RK_MPI_VENC_CreateChn(kChJpeg, &a) != RK_SUCCESS) {
        LOGW(TAG, "JPEG channel unavailable (snapshots disabled)");
        return false;
    }
    VENC_JPEG_PARAM_S jp;
    memset(&jp, 0, sizeof(jp));
    jp.u32Qfactor = g_jpeg_quality;
    RK_MPI_VENC_SetJpegParam(kChJpeg, &jp);
    VENC_CHN_BUF_WRAP_S w;
    memset(&w, 0, sizeof(w));
    w.bEnable = RK_TRUE;
    w.u32BufLine = std::max(128, main.h / 6);
    RK_MPI_VENC_SetChnBufWrapAttr(kChJpeg, &w);
    // combo: takes its input from the main encoder's source, only when asked for a picture
    VENC_COMBO_ATTR_S combo;
    memset(&combo, 0, sizeof(combo));
    combo.bEnable = RK_TRUE;
    combo.s32ChnId = kChMain;
    RK_MPI_VENC_SetComboAttr(kChJpeg, &combo);
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = 1;
    RK_MPI_VENC_StartRecvFrame(kChJpeg, &recv);
    // the first picture is produced right away; drop it
    VENC_STREAM_S st;
    VENC_PACK_S pack;
    st.pstPack = &pack;
    if (RK_MPI_VENC_GetStream(kChJpeg, &st, 1000) == RK_SUCCESS) RK_MPI_VENC_ReleaseStream(kChJpeg, &st);
    return true;
}

static bool g_jpeg_ok = false;

bool snapshot(std::string &jpeg, int timeout_ms) {
    if (!g_run || !g_jpeg_ok) return false;
    std::lock_guard<std::mutex> lk(g_jpeg_mu);
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = 1;
    RK_MPI_VENC_StartRecvFrame(kChJpeg, &recv);
    VENC_STREAM_S st;
    VENC_PACK_S pack;
    st.pstPack = &pack;
    if (RK_MPI_VENC_GetStream(kChJpeg, &st, timeout_ms) != RK_SUCCESS) return false;
    const char *p = (const char *)RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
    jpeg.assign(p, pack.u32Len);
    RK_MPI_VENC_ReleaseStream(kChJpeg, &st);
    return !jpeg.empty();
}

// ---------------------------------------------------------------- threads

static void venc_loop(int ch) {
    prctl(PR_SET_NAME, ch == kChMain ? "venc-main" : "venc-sub", 0, 0, 0);
    Stream *out = stream_by_index(ch);
    const StreamCfg &cfg = ch == kChMain ? g_main : g_sub;
    VENC_STREAM_S st;
    VENC_PACK_S pack;
    st.pstPack = &pack;
    while (g_run) {
        if (RK_MPI_VENC_GetStream(ch, &st, 200) != RK_SUCCESS) continue;
        Packet p;
        p.codec = cfg.codec;
        const char *d = (const char *)RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
        p.data = std::make_shared<std::string>(d, pack.u32Len);
        RK_MPI_VENC_ReleaseStream(ch, &st);  // copy out first so the encoder never waits on us
        if (cfg.codec == Codec::H265)
            p.key = pack.DataType.enH265EType == H265E_NALU_IDRSLICE || pack.DataType.enH265EType == H265E_NALU_ISLICE;
        else
            p.key = pack.DataType.enH264EType == H264E_NALU_IDRSLICE || pack.DataType.enH264EType == H264E_NALU_ISLICE;
        // PTS from the MPI is the capture time in microseconds (CLOCK_MONOTONIC based)
        p.pts_us = (int64_t)pack.u64PTS;
        out->publish(p);
    }
}

static void sub_loop() {
    prctl(PR_SET_NAME, "vi-sub", 0, 0, 0);
    VIDEO_FRAME_INFO_S f;
    int n = 0;
    while (g_run) {
        if (RK_MPI_VI_GetChnFrame(kPipe, kChSub, &f, 500) != RK_SUCCESS) continue;
        MB_BLK blk = f.stVFrame.pMbBlk;
        FrameView v;
        v.nv12 = (uint8_t *)RK_MPI_MB_Handle2VirAddr(blk);
        v.w = f.stVFrame.u32Width;
        v.h = f.stVFrame.u32Height;
        v.stride = f.stVFrame.u32VirWidth ? f.stVFrame.u32VirWidth : v.w;
        v.pts_us = (int64_t)f.stVFrame.u64PTS;
        v.fd = RK_MPI_MB_Handle2Fd(blk);
        RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);  // CPU reads (invalidate)
        bool dirty = false;
        int64_t t0 = now_us();
        isp::feed_frame(v.nv12, v.w, v.h, v.stride);
        int64_t t1 = now_us();
        {
            std::lock_guard<std::mutex> lk(g_hook_mu);
            for (auto &h : g_hooks) dirty |= h.second(v);
        }
        int64_t t2 = now_us();
        if (dirty) RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);  // CPU writes (clean) before the encoder reads
        if (g_sub.enabled) RK_MPI_VENC_SendFrame(kChSub, &f, 200);
        RK_MPI_VI_ReleaseChnFrame(kPipe, kChSub, &f);
        // where the capture thread's time goes (shown in the video status)
        auto avg = [](std::atomic<float> &a, int64_t us) { a = a.load() ? a.load() * 0.95f + us / 1000.f * 0.05f : us / 1000.f; };
        avg(g_cap_isp_ms, t1 - t0);
        avg(g_cap_hooks_ms, t2 - t1);
        avg(g_cap_total_ms, now_us() - t0);
        if (++n == 450)
            LOGI(TAG, "capture thread per frame: exposure stats %.2f ms, hooks %.2f ms, total %.2f ms", g_cap_isp_ms.load(),
                 g_cap_hooks_ms.load(), g_cap_total_ms.load());
    }
}

// ---------------------------------------------------------------- OSD (text overlays)

static int osd_font_px(int stream_h, const std::string &size) {
    float k = size == "small" ? 0.026f : size == "large" ? 0.05f : 0.036f;
    return std::max(14, (int)(stream_h * k));
}

// Render text into an ARGB8888 bitmap (white with dark outline, transparent background).
static void render_text(const std::string &text, int px, std::vector<uint32_t> &argb, int &w, int &h) {
    double scale = px / 22.0;
    int thick = std::max(1, px / 12), base = 0;
    cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thick, &base);
    w = (ts.width + 8 + 15) & ~15;
    h = (ts.height + base + 8 + 15) & ~15;
    cv::Mat m(h, w, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    cv::Point org(4, 4 + ts.height);
    cv::putText(m, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(16, 16, 16, 200), thick + 2, cv::LINE_AA);
    cv::putText(m, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255, 255, 255, 255), thick, cv::LINE_AA);
    // OpenCV BGRA bytes == little-endian ARGB8888 words
    argb.assign((uint32_t *)m.data, (uint32_t *)m.data + (size_t)w * h);
}

static void osd_place(const std::string &pos, int sw, int sh, int w, int h, int &x, int &y) {
    int m = std::max(16, sw / 60);
    x = pos.find("right") != std::string::npos ? sw - w - m : m;
    y = pos.find("bottom") != std::string::npos ? sh - h - m : m;
    x = std::max(0, x) & ~15;
    y = std::max(0, y) & ~15;
}

// OSD regions use ARGB1555 (half the media memory of ARGB8888). Region size is fixed at creation, so a region
// is created once per element with room for longer text. If the media memory is exhausted, attaching fails:
// the element is retried later and the region is never destroyed in that state (the MPI crashes on it).
struct OsdRegion {
    bool created = false, attached = false;
    int w = 0, h = 0;
    int64_t retry_at = 0;
};
static OsdRegion g_osd_rgn[4];
// after a failed attach the MPI region module is unreliable (later calls crash): stop using it
static bool g_rgn_broken = false;

static void to_argb1555(const std::vector<uint32_t> &argb, std::vector<uint16_t> &out) {
    out.resize(argb.size());
    for (size_t i = 0; i < argb.size(); ++i) {
        uint32_t p = argb[i];
        uint16_t a = (p >> 24) >= 128 ? 0x8000 : 0;
        out[i] = a | (((p >> 19) & 0x1f) << 10) | (((p >> 11) & 0x1f) << 5) | ((p >> 3) & 0x1f);
    }
}

static void osd_draw(int handle, int venc, const std::string &text, const std::string &pos, int px, int sw, int sh) {
    if (g_rgn_broken) return;
    int idx = handle - kOsdBase;
    OsdRegion &rg = g_osd_rgn[idx];
    MPP_CHN_S chn;
    chn.enModId = RK_ID_VENC;
    chn.s32DevId = 0;
    chn.s32ChnId = venc;
    RGN_CHN_ATTR_S ca;
    memset(&ca, 0, sizeof(ca));
    ca.enType = OVERLAY_RGN;
    ca.unChnAttr.stOverlayChn.u32BgAlpha = 0;
    ca.unChnAttr.stOverlayChn.u32FgAlpha = 255;
    ca.unChnAttr.stOverlayChn.u32Layer = idx;
    if (text.empty()) {
        // hide instead of destroying (cheap, and safe)
        if (rg.attached) {
            RK_MPI_RGN_GetDisplayAttr(handle, &chn, &ca);
            ca.bShow = RK_FALSE;
            RK_MPI_RGN_SetDisplayAttr(handle, &chn, &ca);
        }
        return;
    }
    std::vector<uint32_t> argb;
    int w, h;
    render_text(text, px, argb, w, h);
    if (!rg.created) {
        // room for longer text (dates change width, names get edited) so the region is never recreated
        rg.w = std::min((sw - 32) & ~15, ((w * 3 / 2) + 15) & ~15);
        rg.h = h;
        RGN_ATTR_S ra;
        memset(&ra, 0, sizeof(ra));
        ra.enType = OVERLAY_RGN;
        ra.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB1555;
        ra.unAttr.stOverlay.u32CanvasNum = 2;
        ra.unAttr.stOverlay.stSize.u32Width = rg.w;
        ra.unAttr.stOverlay.stSize.u32Height = rg.h;
        if (RK_MPI_RGN_Create(handle, &ra) != RK_SUCCESS) {
            LOGW(TAG, "OSD region %d create failed", handle);
            return;
        }
        rg.created = true;
    }
    if (!rg.attached) {
        if (now_ms() < rg.retry_at) return;
        int x, y;
        osd_place(pos, sw, sh, rg.w, rg.h, x, y);
        ca.bShow = RK_TRUE;
        ca.unChnAttr.stOverlayChn.stPoint.s32X = x;
        ca.unChnAttr.stOverlayChn.stPoint.s32Y = y;
        if (RK_MPI_RGN_AttachToChn(handle, &chn, &ca) != RK_SUCCESS) {
            LOGE(TAG, "OSD disabled: no media memory left for it (lower a stream resolution or the AI load)");
            g_rgn_broken = true;
            return;
        }
        rg.attached = true;
    } else {
        // position may have changed in the settings
        int x, y;
        osd_place(pos, sw, sh, rg.w, rg.h, x, y);
        RK_MPI_RGN_GetDisplayAttr(handle, &chn, &ca);
        if (!ca.bShow || ca.unChnAttr.stOverlayChn.stPoint.s32X != x || ca.unChnAttr.stOverlayChn.stPoint.s32Y != y) {
            ca.bShow = RK_TRUE;
            ca.unChnAttr.stOverlayChn.stPoint.s32X = x;
            ca.unChnAttr.stOverlayChn.stPoint.s32Y = y;
            RK_MPI_RGN_SetDisplayAttr(handle, &chn, &ca);
        }
    }
    // pad/crop the text bitmap to the region (transparent)
    int rw = rg.w, cw = std::min(w, rw), ch = std::min(h, rg.h);
    std::vector<uint32_t> full((size_t)rw * rg.h, 0);
    bool right = pos.find("right") != std::string::npos;
    int off = right ? rw - cw : 0;
    for (int r = 0; r < ch; ++r) memcpy(&full[(size_t)r * rw + off], &argb[(size_t)r * w], cw * 4);
    std::vector<uint16_t> px16;
    to_argb1555(full, px16);
    BITMAP_S bm;
    bm.enPixelFormat = RK_FMT_ARGB1555;
    bm.u32Width = rw;
    bm.u32Height = rg.h;
    bm.pData = px16.data();
    RK_MPI_RGN_SetBitMap(handle, &bm);
}

static void osd_refresh(bool force) {
    static std::string last_time[2];
    json o;
    {
        std::lock_guard<std::mutex> lk(g_osd_mu);
        o = g_osd_cfg;
    }
    json tcfg = o.value("time", json::object()), ncfg = o.value("name", json::object());
    std::string size = jget<std::string>(o, "size", "medium");
    bool master = jget<bool>(o, "enabled", true);  // one switch for everything drawn on the picture
    std::string now = master && jget<bool>(tcfg, "enabled", true)
                          ? format_time(time(nullptr), jget<std::string>(tcfg, "format", "%Y-%m-%d %H:%M:%S").c_str())
                          : "";
    bool name_on = master && jget<bool>(ncfg, "enabled", true);
    std::string name = name_on ? jget<std::string>(ncfg, "text", "") : "";
    if (name.empty() && name_on) name = g_settings.get("device").value("name", "");
    const StreamCfg *s[2] = {&g_main, &g_sub};
    int venc[2] = {kChMain, kChSub};
    for (int i = 0; i < 2; ++i) {
        if (!s[i]->enabled) continue;
        int px = osd_font_px(s[i]->h, size);
        if (force || now != last_time[i] || !g_osd_rgn[i * 2].attached)
            osd_draw(kOsdBase + i * 2, venc[i], now, jget<std::string>(tcfg, "position", "top-left"), px, s[i]->w, s[i]->h);
        if (force || (!name.empty() && !g_osd_rgn[i * 2 + 1].attached))
            osd_draw(kOsdBase + i * 2 + 1, venc[i], name, jget<std::string>(ncfg, "position", "bottom-right"), px, s[i]->w,
                     s[i]->h);
        last_time[i] = now;
    }
}

static void osd_detach_all() {
    if (g_rgn_broken) return;
    for (int i = 0; i < 4; ++i) {
        OsdRegion &rg = g_osd_rgn[i];
        if (rg.attached) {
            MPP_CHN_S chn;
            chn.enModId = RK_ID_VENC;
            chn.s32DevId = 0;
            chn.s32ChnId = i < 2 ? kChMain : kChSub;
            RK_MPI_RGN_DetachFromChn(kOsdBase + i, &chn);
        }
        if (rg.created && rg.attached) RK_MPI_RGN_Destroy(kOsdBase + i);
        // a region whose attach failed is left alone: its handle is reused on the next start
        if (rg.attached || !rg.created) rg = OsdRegion();
        else rg.retry_at = 0;
    }
}

static void osd_loop() {
    prctl(PR_SET_NAME, "osd", 0, 0, 0);
    osd_refresh(true);
    while (g_run) {
        // wake just after each second boundary so the clock ticks on time
        int64_t ms = wall_ms() % 1000;
        usleep((1000 - ms + 20) * 1000);
        if (g_run) osd_refresh(false);
    }
}

void apply_osd(const json &osd) {
    {
        std::lock_guard<std::mutex> lk(g_osd_mu);
        g_osd_cfg = osd;
    }
    if (g_run) osd_refresh(true);
}

// ---------------------------------------------------------------- privacy masks

void apply_masks(const json &masks) {
    if (!g_run || g_rgn_broken) return;
    MPP_CHN_S chn;
    chn.enModId = RK_ID_VI;
    chn.s32DevId = 0;
    chn.s32ChnId = VI_MAX_CHN_NUM;  // pipe level: covers every stream and snapshot
    for (int i = 0; i < g_mask_count; ++i) {
        RK_MPI_RGN_DetachFromChn(kMaskBase + i, &chn);
        RK_MPI_RGN_Destroy(kMaskBase + i);
    }
    g_mask_count = 0;
    if (!masks.is_array()) return;
    // mask coordinates are normalised (0..1) to the picture; the cover works in sensor pixels
    int W = g_main.w, H = g_main.h;
    for (auto &m : masks) {
        if (g_mask_count >= 8) break;
        if (!jget<bool>(m, "enabled", true)) continue;
        int h = kMaskBase + g_mask_count;
        RGN_ATTR_S ra;
        memset(&ra, 0, sizeof(ra));
        ra.enType = COVER_RGN;
        if (RK_MPI_RGN_Create(h, &ra) != RK_SUCCESS) continue;
        RGN_CHN_ATTR_S ca;
        memset(&ca, 0, sizeof(ca));
        ca.bShow = RK_TRUE;
        ca.enType = COVER_RGN;
        auto al = [](int v) { return (v + 15) & ~15; };
        int x = al((int)(jget<float>(m, "x", 0) * W)), y = al((int)(jget<float>(m, "y", 0) * H));
        int w = al((int)(jget<float>(m, "w", 0.1f) * W)), hh = al((int)(jget<float>(m, "h", 0.1f) * H));
        w = std::max(16, std::min(w, W - x));
        hh = std::max(16, std::min(hh, H - y));
        ca.unChnAttr.stCoverChn.stRect.s32X = x;
        ca.unChnAttr.stCoverChn.stRect.s32Y = y;
        ca.unChnAttr.stCoverChn.stRect.u32Width = w;
        ca.unChnAttr.stCoverChn.stRect.u32Height = hh;
        ca.unChnAttr.stCoverChn.u32Color = 0x101010;
        ca.unChnAttr.stCoverChn.u32Layer = g_mask_count;
        if (RK_MPI_RGN_AttachToChn(h, &chn, &ca) != RK_SUCCESS) {
            RK_MPI_RGN_Destroy(h);
            continue;
        }
        ++g_mask_count;
    }
    LOGI(TAG, "%d privacy mask(s) active", g_mask_count);
}

// ---------------------------------------------------------------- lifecycle

void request_idr(int stream) {
    if (!g_run) return;
    int ch = stream == 0 ? kChMain : kChSub;
    // an encoder fed by the app (the sub stream) refuses an instant request ("not supported"): the next frame then
    int r = RK_MPI_VENC_RequestIDR(ch, RK_TRUE);
    if (r != RK_SUCCESS) r = RK_MPI_VENC_RequestIDR(ch, RK_FALSE);
    if (r != RK_SUCCESS) LOGW(TAG, "keyframe request on the %s stream failed: %#x", stream == 0 ? "main" : "sub", r);
}

static void teardown() {
    g_run = false;
    if (g_th_osd.joinable()) g_th_osd.join();
    if (g_th_sub.joinable()) g_th_sub.join();
    for (auto &t : g_th_venc)
        if (t.joinable()) t.join();
    osd_detach_all();
    MPP_CHN_S vi{RK_ID_VI, 0, kChMain}, ve{RK_ID_VENC, 0, kChMain};
    RK_MPI_SYS_UnBind(&vi, &ve);
    if (g_jpeg_ok) {
        RK_MPI_VENC_StopRecvFrame(kChJpeg);
        RK_MPI_VENC_DestroyChn(kChJpeg);
        g_jpeg_ok = false;
    }
    for (int ch : {kChMain, kChSub}) {
        RK_MPI_VENC_StopRecvFrame(ch);
        RK_MPI_VENC_DestroyChn(ch);
    }
    // masks are attached to the VI pipe
    MPP_CHN_S pipe{RK_ID_VI, 0, VI_MAX_CHN_NUM};
    for (int i = 0; i < g_mask_count; ++i) {
        RK_MPI_RGN_DetachFromChn(kMaskBase + i, &pipe);
        RK_MPI_RGN_Destroy(kMaskBase + i);
    }
    g_mask_count = 0;
    RK_MPI_VI_DisableChn(kPipe, kChSub);
    RK_MPI_VI_DisableChn(kPipe, kChMain);
    // Rockchip order: the 3A engine stops after the VI streams and starts before them. Re-enabling the streams
    // under a running AIQ leaves the ISP without its white-balance gains (green picture) and the AE frozen
    // until a reboot.
    isp::stop();
}

struct Transition {
    Transition() { g_trans_since = now_ms(); }
    ~Transition() { g_trans_since = 0; }
};

static bool start_impl(const json &v, const json &osd, const json &masks) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_run) return true;
    Transition tr;
    g_sensor_fps = std::max(5, std::min(30, jget<int>(v, "sensor_fps", 25)));
    g_main = parse_stream(v.value("main", json::object()), g_sensor_fps);
    g_sub = parse_stream(v.value("sub", json::object()), g_sensor_fps);
    fit_to_sensor(g_main, "main");
    fit_to_sensor(g_sub, "sub");
    g_jpeg_quality = std::max(10, std::min(99, jget<int>(v.value("jpeg", json::object()), "quality", 80)));

    sys_init();
    if (!g_sys_init) return false;
    // same IQ files; picks up a changed sensor frame rate. Right after a stop the ISP device can still be busy.
    bool isp_ok = false;
    for (int i = 0; i < 3 && !(isp_ok = isp::start("", g_sensor_fps)); ++i) usleep(400 * 1000);
    if (!isp_ok) {
        LOGE(TAG, "ISP did not start");
        return false;
    }
    long cma0 = meminfo_kb("CmaAllocated");
    // main path always runs (the ISP's main output); the encoder is skipped when the stream is disabled
    if (!vi_chn_init(kChMain, g_main, 2, 0, true)) {
        teardown();
        return false;
    }
    if (g_main.enabled) {
        if (!venc_init(kChMain, g_main, true)) {
            teardown();
            return false;
        }
        MPP_CHN_S vi{RK_ID_VI, 0, kChMain}, ve{RK_ID_VENC, 0, kChMain};
        if (RK_MPI_SYS_Bind(&vi, &ve) != RK_SUCCESS) LOGE(TAG, "bind VI->VENC failed");
        g_jpeg_ok = jpeg_init(g_main);
    }
    // sub path: the app holds a frame only for the few ms the hooks take
    if (!vi_chn_init(kChSub, g_sub, 2, 1, false)) {
        teardown();
        return false;
    }
    if (g_sub.enabled && !venc_init(kChSub, g_sub, false)) {
        teardown();
        return false;
    }
    g_main_stream.reset(g_main.codec, g_main.w, g_main.h, g_main.fps);
    g_sub_stream.reset(g_sub.codec, g_sub.w, g_sub.h, g_sub.fps);
    g_main_stream.request_idr = [] { request_idr(0); };
    g_sub_stream.request_idr = [] { request_idr(1); };
    {
        std::lock_guard<std::mutex> lk2(g_osd_mu);
        g_osd_cfg = osd;
    }
    g_run = true;
    if (g_main.enabled) g_th_venc[0] = std::thread(venc_loop, kChMain);
    if (g_sub.enabled) g_th_venc[1] = std::thread(venc_loop, kChSub);
    g_th_sub = std::thread(sub_loop);
    g_th_osd = std::thread(osd_loop);
    apply_masks(masks);
    LOGI(TAG, "main %s %dx%d@%d %dkbps%s | sub %s %dx%d@%d %dkbps | media memory +%ld KB", codec_name(g_main.codec),
         g_main.w, g_main.h, g_main.fps, g_main.bitrate, g_main.enabled ? "" : " (off)", codec_name(g_sub.codec),
         g_sub.w, g_sub.h, g_sub.fps, g_sub.bitrate, meminfo_kb("CmaAllocated") - cma0);
    return true;
}

bool start(const json &v, const json &osd, const json &masks) {
    bool ok = start_impl(v, osd, masks);
    // a failed start is a stall too: the watchdog resets the board if nothing brings the video back
    if (ok) g_fail_since = 0;
    else if (!g_fail_since) g_fail_since = now_ms();
    return ok;
}

void stop() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_run) return;
    Transition tr;
    teardown();
}

bool restart(const json &v, const json &osd, const json &masks) {
    stop();
    return start(v, osd, masks);
}

bool running() { return g_run; }

int64_t stalled_ms() {
    int64_t since = g_trans_since;
    if (since) return now_ms() - since;
    int64_t failed = g_fail_since;
    if (failed) return now_ms() - failed;
    if (!g_run) return 0;
    // the sub path always runs; the main path only when enabled
    int64_t idle = g_sub.enabled ? g_sub_stream.idle_ms() : 0;
    if (g_main.enabled) idle = std::max(idle, g_main_stream.idle_ms());
    return idle;
}

json status() {
    auto s = [](const Stream &st, const StreamCfg &c) {
        return json{{"enabled", c.enabled},   {"codec", codec_name(c.codec)}, {"width", c.w},
                    {"height", c.h},          {"fps", c.fps},                 {"bitrate", c.bitrate},
                    {"kbps", std::round(st.kbps())}, {"measured_fps", std::round(st.measured_fps() * 10.0) / 10.0},
                    {"clients", st.subscribers()}};
    };
    json sub = s(g_sub_stream, g_sub);
    // the boxes are in the picture itself: viewers of the sub stream must not draw their own on top
    sub["ai_overlay"] = jget<bool>(g_settings.get("video").value("sub", json::object()), "ai_overlay", false);
    auto r2 = [](float v) { return std::round(v * 100.0) / 100.0; };
    return {{"running", g_run.load()}, {"main", s(g_main_stream, g_main)}, {"sub", sub}, {"sensor_fps", g_sensor_fps},
            {"capture", {{"exposure_stats_ms", r2(g_cap_isp_ms)}, {"hooks_ms", r2(g_cap_hooks_ms)}, {"frame_ms", r2(g_cap_total_ms)}}}};
}

}  // namespace video
