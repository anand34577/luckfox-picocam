#pragma once
#include <string>
#include <vector>

#include "core/config.h"
#include "imgproc.h"
#include "rknn_api.h"

// One RKNN model using the RV1103/RV1106 zero-copy API (NHWC uint8 input, native int8 outputs).
class NpuModel {
public:
    ~NpuModel();
    // shared_internal: allocate internal (activation) memory outside, see npu_share_internal()
    bool load(const std::string &path, bool shared_internal = true);
    // Fills the input tensor with `roi` of the frame, scaled (letterboxed with grey when keep_aspect) and converted
    // to RGB/BGR. Uses the RGA 2D engine (no CPU time; it writes straight into the NPU's buffer) and falls back to
    // the CPU path for anything the RGA cannot do. Returns how model coordinates map back to the frame.
    Letterbox set_input(const Frame &f, RectF roi, bool keep_aspect, bool bgr);
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
    bool rga_input(const Frame &f, const Letterbox &lb, bool bgr);
    std::string path_;
    std::string pad_key_;  // padding the input tensor currently holds (the RGA path refills it only when it changes)
    int rga_fail_ = 0;     // consecutive RGA failures; after 3 this model stays on the CPU path
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

// Preprocessing backend: "auto" (RGA, verified against the CPU result on first use, CPU fallback) or "cpu".
void npu_set_preprocess(const std::string &mode);
// Whether the AI frames are in media memory (dma-buf), the only memory the RGA can read. Without it every model
// prepares its input on the CPU; `why` is shown in the AI status.
void npu_rga_frames(bool available, const std::string &why = "");
// Copies an NV12 picture (stride x h) between two dma-bufs on the RGA, so the capture thread spends no CPU on the AI's
// frame copy. False if the RGA is unusable or the copy failed: the caller copies on the CPU instead.
bool rga_copy_nv12(int src_fd, int dst_fd, int stride, int h);
void rga_copy_disable(const char *why);  // the copy was checked and is wrong: CPU copies from now on
// Where the AI time goes: preprocessing backend, average preprocessing / NPU inference time per model run.
json npu_stats();
