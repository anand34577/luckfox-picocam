#pragma once
#include <string>
#include <vector>

#include "rknn_api.h"

// One RKNN model using the RV1103/RV1106 zero-copy API (NHWC uint8 input, native int8 outputs).
class NpuModel {
public:
    ~NpuModel();
    // shared_internal: allocate internal (activation) memory outside, see npu_share_internal()
    bool load(const std::string &path, bool shared_internal = true);
    bool run();

    int in_w() const { return in_w_; }
    int in_h() const { return in_h_; }
    int in_c() const { return in_c_; }
    int in_stride() const { return in_wstride_ * in_c_; }  // bytes per input row
    uint8_t *input() const { return (uint8_t *)in_mem_->virt_addr; }

    int num_outputs() const { return (int)out_attrs_.size(); }
    const rknn_tensor_attr &out_attr(int i) const { return out_attrs_[i]; }
    int8_t *output(int i) const { return (int8_t *)outs_[i]->virt_addr; }
    float deq(int i, int8_t v) const { return ((float)v - out_attrs_[i].zp) * out_attrs_[i].scale; }
    // dequantised element `idx` (byte offset) of output i, honouring int8/uint8/fp16 output types
    float outf(int i, size_t idx) const;
    // float threshold -> quantised int8 value (for fast pre-filtering)
    int8_t qnt(int i, float f) const;

    uint32_t internal_size() const { return internal_size_; }
    rknn_context ctx() const { return ctx_; }
    const std::string &path() const { return path_; }

private:
    friend bool npu_share_internal(std::vector<NpuModel *> models);
    bool bind_io();  // allocate input/output tensors (must follow rknn_set_internal_mem)
    std::string path_;
    rknn_context ctx_ = 0;
    rknn_tensor_mem *in_mem_ = nullptr;
    std::vector<rknn_tensor_mem *> outs_;
    std::vector<rknn_tensor_attr> out_attrs_;
    rknn_tensor_attr in_attr_{};
    int in_w_ = 0, in_h_ = 0, in_c_ = 3, in_wstride_ = 0;
    uint32_t internal_size_ = 0;
    bool dumped_ = false;
    bool shared_ = false;
    rknn_tensor_mem *internal_ = nullptr;
    rknn_tensor_mem *internal_owner_ = nullptr;  // set on the model that allocated the shared block
    rknn_tensor_mem *weight_ = nullptr;
};

// Models in one profile run one after another on the same thread, so they can share a single
// internal-memory block sized for the largest model. Saves several MB of CMA.
bool npu_share_internal(std::vector<NpuModel *> models);
