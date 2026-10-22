#include "npu.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "aicommon.h"
#include "im2d.h"

// ---------------------------------------------------------------- preprocessing backend and timing

static std::atomic<bool> g_pre_cpu{false};   // the user chose the CPU path
static std::atomic<int> g_rga_state{0};      // 0 = not verified yet, 1 = verified, -1 = unusable on this board
static std::atomic<bool> g_rga_swap{false};  // this librga's RGB byte order is the reverse of OpenCV's
static std::mutex g_stat_mu;
static double g_pre_ms = 0, g_npu_ms = 0;  // moving averages per model run
static int64_t g_rga_runs = 0, g_cpu_runs = 0;
static std::string g_rga_note;
static std::atomic<bool> g_rga_frames{false};  // the AI frames are in media memory (the only memory the RGA can read)
static std::string g_frames_note;

void npu_set_preprocess(const std::string &mode) { g_pre_cpu = mode == "cpu"; }

void npu_rga_frames(bool available, const std::string &why) {
    std::lock_guard<std::mutex> lk(g_stat_mu);
    g_rga_frames = available;
    g_frames_note = available ? "" : why;
}

static void note_time(double *avg, double ms) {
    std::lock_guard<std::mutex> lk(g_stat_mu);
    *avg = *avg ? *avg * 0.9 + ms * 0.1 : ms;
}

json npu_stats() {
    std::lock_guard<std::mutex> lk(g_stat_mu);
    std::string be = g_pre_cpu            ? "cpu"
                     : !g_rga_frames      ? "cpu (no media memory for the RGA)"
                     : g_rga_state == 1   ? "rga"
                     : g_rga_state == 0   ? "rga (checking)"
                                          : "cpu (rga unavailable)";
    int64_t all = g_rga_runs + g_cpu_runs;
    return {{"preprocess", be},
            {"preprocess_ms", std::round(g_pre_ms * 100.0) / 100.0},
            {"inference_ms", std::round(g_npu_ms * 100.0) / 100.0},
            {"rga_share_pct", all ? (int)(g_rga_runs * 100 / all) : 0},
            {"note", g_rga_note.empty() ? g_frames_note : g_rga_note}};
}

NpuModel::~NpuModel() {
    if (!ctx_) return;
    if (in_mem_) rknn_destroy_mem(ctx_, in_mem_);
    for (auto *m : outs_) rknn_destroy_mem(ctx_, m);
    if (internal_) rknn_destroy_mem(ctx_, internal_);
    if (internal_owner_) rknn_destroy_mem(ctx_, internal_owner_);
    if (weight_) rknn_destroy_mem(ctx_, weight_);
    rknn_destroy(ctx_);
}

bool NpuModel::load(const std::string &path, bool shared_internal) {
    path_ = path;
    if (getenv("EDGEAI_NO_SHARE")) shared_internal = false;
    shared_ = shared_internal;
    int ret = rknn_init(&ctx_, (void *)path.c_str(), 0, shared_internal ? RKNN_FLAG_MEM_ALLOC_OUTSIDE : 0, nullptr);
    if (ret < 0) {
        LOGE("ai", "rknn_init(%s) failed: %d", path.c_str(), ret);
        ctx_ = 0;
        return false;
    }
    rknn_input_output_num io{};
    rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));

    in_attr_.index = 0;
    rknn_query(ctx_, RKNN_QUERY_NATIVE_INPUT_ATTR, &in_attr_, sizeof(in_attr_));
    in_attr_.type = RKNN_TENSOR_UINT8;  // normalisation is fused into the model
    in_attr_.fmt = RKNN_TENSOR_NHWC;
    in_h_ = in_attr_.dims[1];
    in_w_ = in_attr_.dims[2];
    in_c_ = in_attr_.dims[3];
    in_wstride_ = in_attr_.w_stride ? in_attr_.w_stride : in_w_;

    out_attrs_.resize(io.n_output);
    for (uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr &a = out_attrs_[i];
        memset(&a, 0, sizeof(a));
        a.index = i;
        rknn_query(ctx_, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &a, sizeof(a));
    }

    rknn_mem_size ms{};
    rknn_query(ctx_, RKNN_QUERY_MEM_SIZE, &ms, sizeof(ms));
    internal_size_ = ms.total_internal_size;
    if (shared_internal) {
        // MEM_ALLOC_OUTSIDE means the caller provides *all* memory, weights included
        weight_ = rknn_create_mem(ctx_, ms.total_weight_size);
        if (!weight_ || rknn_set_weight_mem(ctx_, weight_) < 0) {
            LOGE("ai", "%s: weight mem (%u KB) failed", path.c_str(), ms.total_weight_size / 1024);
            return false;
        }
    }

    LOGI("ai", "model %s: in %dx%dx%d (wstride %d), %u outputs, weights %u KB, internal %u KB", path.c_str(), in_w_, in_h_,
         in_c_, in_wstride_, io.n_output, ms.total_weight_size / 1024, ms.total_internal_size / 1024);
    for (auto &a : out_attrs_)
        LOGI("ai", "  out %u: dims [%u %u %u %u] n_dims %u size_ws %u zp %d scale %f", a.index, a.dims[0], a.dims[1],
             a.dims[2], a.dims[3], a.n_dims, a.size_with_stride, a.zp, a.scale);
    return shared_internal ? true : bind_io();
}

bool NpuModel::bind_io() {
    in_mem_ = rknn_create_mem(ctx_, in_attr_.size_with_stride);
    if (!in_mem_ || rknn_set_io_mem(ctx_, in_mem_, &in_attr_) < 0) {
        LOGE("ai", "%s: input mem failed", path_.c_str());
        return false;
    }
    for (auto &a : out_attrs_) {
        rknn_tensor_mem *m = rknn_create_mem(ctx_, a.size_with_stride);
        if (!m || rknn_set_io_mem(ctx_, m, &a) < 0) {
            LOGE("ai", "%s: output %u mem failed", path_.c_str(), a.index);
            return false;
        }
        outs_.push_back(m);
    }
    return true;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {  // subnormal
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) man <<= 1, --exp;
            bits = sign | (exp << 23) | ((man & 0x3ff) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (man << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

float NpuModel::outf(int i, size_t idx) const {
    const rknn_tensor_attr &a = out_attrs_[i];
    const void *p = outs_[i]->virt_addr;
    switch (a.type) {
        case RKNN_TENSOR_UINT8: return ((float)((const uint8_t *)p)[idx] - a.zp) * a.scale;
        case RKNN_TENSOR_FLOAT32: return ((const float *)p)[idx];
        case RKNN_TENSOR_FLOAT16: return half_to_float(((const uint16_t *)p)[idx]);
        default: return ((float)((const int8_t *)p)[idx] - a.zp) * a.scale;
    }
}

int8_t NpuModel::qnt(int i, float f) const {
    float v = f / out_attrs_[i].scale + out_attrs_[i].zp;
    return (int8_t)std::max(-128.f, std::min(127.f, v));
}

bool NpuModel::run() {
    static const char *dump = getenv("EDGEAI_DUMP");
    if (dump && !dumped_) {
        rknn_sdk_version v{};
        rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &v, sizeof(v));
        LOGI("ai", "rknn api %s, driver %s", v.api_version, v.drv_version);
        for (size_t i = 0; i < outs_.size(); ++i) {  // sentinel: shows whether the NPU writes outputs
            memset(outs_[i]->virt_addr, 0x55, out_attrs_[i].size_with_stride);
            rknn_mem_sync(ctx_, outs_[i], RKNN_MEMORY_SYNC_TO_DEVICE);
        }
    }
    // tensors live in cached memory: flush what the CPU wrote, invalidate what the NPU wrote
    rknn_mem_sync(ctx_, in_mem_, RKNN_MEMORY_SYNC_TO_DEVICE);
    int64_t t0 = now_us();
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) {
        LOGE("ai", "rknn_run(%s) failed: %d", path_.c_str(), ret);
        return false;
    }
    note_time(&g_npu_ms, (now_us() - t0) / 1000.0);
    for (auto *m : outs_) rknn_mem_sync(ctx_, m, RKNN_MEMORY_SYNC_FROM_DEVICE);

    // EDGEAI_DUMP=/dir: write the first input/outputs of each model for offline debugging
    if (dump && !dumped_) {
        size_t same = 0;
        for (uint32_t k = 0; k < out_attrs_[0].size_with_stride; ++k) same += ((uint8_t *)outs_[0]->virt_addr)[k] == 0x55;
        LOGI("ai", "%s: %zu of %u output bytes untouched by NPU", path_.c_str(), same, out_attrs_[0].size_with_stride);
        dumped_ = true;
        std::string base = std::string(dump) + "/" + path_.substr(path_.find_last_of('/') + 1);
        auto write = [](const std::string &p, const void *d, size_t n) {
            if (FILE *fp = fopen(p.c_str(), "wb")) fwrite(d, 1, n, fp), fclose(fp);
        };
        write(base + ".in", in_mem_->virt_addr, in_attr_.size_with_stride);
        for (size_t i = 0; i < outs_.size(); ++i)
            write(base + ".out" + std::to_string(i), outs_[i]->virt_addr, out_attrs_[i].size_with_stride);
    }
    return true;
}

// The RGA crops, scales and converts NV12 to RGB in one pass and writes straight into the NPU's input buffer, so the
// CPU stays free for decoding, tracking and the web server. Both sides must be dma-bufs in media memory: the
// RV1103/RV1106 RGA has no MMU, and a frame in ordinary memory (virtual address) makes the kernel driver fail and
// corrupt kernel memory on its error path (the board freezes). Such frames always take the CPU path.
bool NpuModel::rga_input(const Frame &f, const Letterbox &lb, bool bgr) {
    if (f.fd < 0 || !in_mem_ || in_mem_->fd < 0 || in_c_ != 3 || rga_fail_ >= 3) return false;
    rga_buffer_t src = wrapbuffer_fd(f.fd, f.w, f.h, RK_FORMAT_YCbCr_420_SP, f.stride, f.h);
    rga_buffer_t dst = wrapbuffer_fd(in_mem_->fd, in_w_, in_h_, bgr ? RK_FORMAT_BGR_888 : RK_FORMAT_RGB_888, in_wstride_, in_h_);
    im_rect sr = {(int)lb.roi.x1, (int)lb.roi.y1, (int)lb.roi.w(), (int)lb.roi.h()};
    im_rect dr = {lb.pad_x, lb.pad_y, lb.out_w, lb.out_h};
    if (imcheck(src, dst, sr, dr) != IM_STATUS_NOERROR) return false;  // this crop/scale is outside what the RGA does
    auto failed = [&](const char *what) {
        if (++rga_fail_ == 3) LOGW("ai", "%s: RGA %s keeps failing, CPU preprocessing for this model", path_.c_str(), what);
        pad_key_.clear();
        return false;
    };
    // grey bars (YOLO letterbox colour 114); the NPU never writes its input, so they are drawn only when they move
    std::string key = std::to_string(lb.pad_x) + "," + std::to_string(lb.pad_y) + "," + std::to_string(lb.out_w) + "," + std::to_string(lb.out_h);
    if (key != pad_key_) {
        im_rect bars[4] = {{0, 0, in_w_, lb.pad_y},
                           {0, lb.pad_y + lb.out_h, in_w_, in_h_ - lb.pad_y - lb.out_h},
                           {0, lb.pad_y, lb.pad_x, lb.out_h},
                           {lb.pad_x + lb.out_w, lb.pad_y, in_w_ - lb.pad_x - lb.out_w, lb.out_h}};
        for (auto &r : bars)
            if (r.width > 0 && r.height > 0 && imfill(dst, r, 0xff727272) != IM_STATUS_SUCCESS) return failed("fill");
        pad_key_ = key;
    }
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));
    im_rect pr = {0, 0, 0, 0};
    if (improcess(src, dst, pat, sr, dr, pr, IM_SYNC) != IM_STATUS_SUCCESS) return failed("convert");
    rga_fail_ = 0;
    return true;
}

Letterbox NpuModel::set_input(const Frame &f, RectF roi, bool keep_aspect, bool bgr) {
    int64_t t0 = now_us();
    Letterbox lb = plan_letterbox(f, roi, in_w_, in_h_, keep_aspect);
    bool hw = !g_pre_cpu && g_rga_frames && g_rga_state >= 0 && rga_input(f, lb, bgr != g_rga_swap);
    if (hw && g_rga_state == 0) {
        // First RGA picture: check it against the CPU conversion (colour order, range, scaling) before trusting it.
        // The CPU result is what this run uses either way.
        size_t n = (size_t)in_stride() * in_h_;
        rknn_mem_sync(ctx_, in_mem_, RKNN_MEMORY_SYNC_FROM_DEVICE);
        std::vector<uint8_t> rga(input(), input() + n);
        nv12_to_rgb(f, roi, input(), in_w_, in_h_, in_stride(), keep_aspect, bgr);
        pad_key_.clear();
        double d = 0, ds = 0, chroma = 0;
        size_t cnt = 0;
        for (int y = lb.pad_y; y < lb.pad_y + lb.out_h; y += 2) {
            const uint8_t *a = rga.data() + (size_t)y * in_stride() + lb.pad_x * 3, *b = input() + (size_t)y * in_stride() + lb.pad_x * 3;
            for (int x = 0; x < lb.out_w; x += 2, a += 6, b += 6) {
                d += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
                ds += std::abs(a[0] - b[2]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[0]);
                chroma += std::abs(b[0] - b[2]);
                cnt += 3;
            }
        }
        if (cnt) d /= cnt, ds /= cnt, chroma /= cnt / 3;
        if (cnt && d > 10 && ds > 10) {
            g_rga_state = -1;
            std::lock_guard<std::mutex> lk(g_stat_mu);
            g_rga_note = "RGA output differs from the CPU conversion (mean error " + std::to_string((int)d) + "): using the CPU";
            LOGW("ai", "%s", g_rga_note.c_str());
        } else if (cnt && chroma >= 6) {  // enough colour in the picture to tell RGB from BGR
            if (ds < d) g_rga_swap = !g_rga_swap;
            g_rga_state = 1;
            LOGI("ai", "preprocessing on the RGA (checked against the CPU: mean error %.1f%s)", std::min(d, ds),
                 ds < d ? ", channel order swapped" : "");
        }
        hw = false;  // this run: the CPU conversion
    } else if (!hw) {
        nv12_to_rgb(f, roi, input(), in_w_, in_h_, in_stride(), keep_aspect, bgr);
        pad_key_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_stat_mu);
        ++(hw ? g_rga_runs : g_cpu_runs);
    }
    note_time(&g_pre_ms, (now_us() - t0) / 1000.0);
    return lb;
}

bool npu_share_internal(std::vector<NpuModel *> models) {
    uint32_t max_size = 0;
    NpuModel *owner = nullptr;
    for (auto *m : models)
        if (m && m->ctx_ && m->shared_ && m->internal_size_ >= max_size) {
            max_size = m->internal_size_;
            owner = m;
        }
    if (!owner) return true;
    rknn_tensor_mem *block = rknn_create_mem(owner->ctx_, max_size);
    if (!block) {
        LOGE("ai", "shared internal alloc of %u KB failed", max_size / 1024);
        return false;
    }
    owner->internal_owner_ = block;
    for (auto *m : models) {
        if (!m || !m->ctx_ || !m->shared_) continue;
        m->internal_ = rknn_create_mem_from_fd(m->ctx_, block->fd, block->virt_addr, m->internal_size_, 0);
        if (!m->internal_ || rknn_set_internal_mem(m->ctx_, m->internal_) < 0) {
            LOGE("ai", "set_internal_mem failed for %s", m->path_.c_str());
            return false;
        }
        if (!m->bind_io()) return false;
    }
    LOGI("ai", "NPU shared internal memory: %u KB for %zu models", max_size / 1024, models.size());
    return true;
}
