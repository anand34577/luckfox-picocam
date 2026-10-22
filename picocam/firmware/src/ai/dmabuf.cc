#include "dmabuf.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include <linux/dma-buf.h>

// <linux/dma-heap.h> is missing from older kernel headers
struct DmaHeapAlloc {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define PC_DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, DmaHeapAlloc)

// Contiguous heaps only: a "system" heap hands out scattered pages, which is exactly what the RGA cannot use.
static const char *kHeaps[] = {"/dev/rk_dma_heap/rk-dma-heap-cma", "/dev/dma_heap/linux,cma", "/dev/dma_heap/cma"};

bool DmaBuf::alloc(size_t bytes) {
    release();
    for (const char *path : kHeaps) {
        int heap = open(path, O_RDWR | O_CLOEXEC);
        if (heap < 0) continue;
        DmaHeapAlloc a;
        memset(&a, 0, sizeof(a));
        a.len = bytes;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        int r = ioctl(heap, PC_DMA_HEAP_IOCTL_ALLOC, &a);
        close(heap);
        if (r < 0) return false;  // the heap exists but has no room: another heap would not be contiguous memory either
        void *va = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)a.fd, 0);
        if (va == MAP_FAILED) {
            close((int)a.fd);
            return false;
        }
        fd_ = (int)a.fd;
        va_ = (uint8_t *)va;
        size_ = bytes;
        return true;
    }
    return false;
}

void DmaBuf::release() {
    if (va_) munmap(va_, size_);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    va_ = nullptr;
    size_ = 0;
}

void DmaBuf::begin_cpu_write() {
    if (fd_ < 0) return;
    dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
    ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
}

void DmaBuf::end_cpu_write() {
    if (fd_ < 0) return;
    dma_buf_sync s = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE};
    ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
}

void DmaBuf::cpu_read_sync() {
    if (fd_ < 0) return;
    dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
}
