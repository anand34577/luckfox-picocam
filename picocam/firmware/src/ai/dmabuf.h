#pragma once
#include <cstddef>
#include <cstdint>

// A buffer in media memory (CMA): one physically contiguous block, shared as a dma-buf fd and mapped for the CPU.
// The RGA of the RV1103/RV1106 has no MMU ("NONE_MMU"): it can only read and write buffers like this one. Handing it
// ordinary (scattered) memory by virtual address makes the kernel driver reject the job and then corrupt the
// kernel's page bookkeeping on its error path, which freezes the board.
class DmaBuf {
public:
    DmaBuf() = default;
    DmaBuf(const DmaBuf &) = delete;
    DmaBuf &operator=(const DmaBuf &) = delete;
    ~DmaBuf() { release(); }

    bool alloc(size_t bytes);  // false: no CMA heap on this board, or no media memory left
    void release();

    int fd() const { return fd_; }
    uint8_t *data() const { return va_; }
    size_t size() const { return size_; }

    // the mapping is cached: bracket CPU writes so the RGA/NPU see them in memory
    void begin_cpu_write();
    void end_cpu_write();

private:
    int fd_ = -1;
    uint8_t *va_ = nullptr;
    size_t size_ = 0;
};
