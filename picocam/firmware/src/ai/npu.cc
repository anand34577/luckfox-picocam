#include "npu.h"

#include <cstring>

#include "aicommon.h"

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
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) {
        LOGE("ai", "rknn_run(%s) failed: %d", path_.c_str(), ret);
        return false;
    }
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
