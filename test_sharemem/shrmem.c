#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#ifndef DMA_HEAP_IOCTL_ALLOC

#include <linux/types.h>

struct dma_heap_allocation_data {
    __u64 len;          // size of allocation
    __u32 fd;           // returned fd
    __u32 fd_flags;     // flags for fd (O_RDWR etc)
    __u32 heap_flags;   // not used yet
    __u32 reserved;     // reserved
};

#define DMA_HEAP_IOC_MAGIC        'H'
#define DMA_HEAP_IOCTL_ALLOC      _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

#endif

#include <string.h>
#include <errno.h>

#define DMA_HEAP_NAME "/dev/dma_heap/carveout_ai-share-memory@a7000000"
#define WATCH_VALUE 592
#define BUFFER_SIZE 4096  // 최소 1 페이지

int main() {
    int heap_fd = -1, buf_fd = -1;
    void *mapped = NULL;
    struct dma_heap_allocation_data alloc_data = {
        .len = BUFFER_SIZE,
        .fd_flags = O_RDWR,
        .heap_flags = 0,
    };

    // 1. dma-heap open
    heap_fd = open(DMA_HEAP_NAME, O_RDWR);
    if (heap_fd < 0) {
        perror("open dma_heap");
        return 1;
    }

    // 2. ioctl로 버퍼 할당
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return 1;
    }
    buf_fd = alloc_data.fd;

    // 3. mmap으로 버퍼 매핑
    mapped = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        close(buf_fd);
        close(heap_fd);
        return 1;
    }

    // 4. 감시 대상 포인터
    volatile int32_t *watch_addr = (volatile int32_t *)mapped;

    // 5. 대기 루프 (DMA_BUF_SYNC_START/END로 캐시 동기화)
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW
    };

    printf("Waiting for value %d...\n", WATCH_VALUE);
    while (1) {
        // 캐시 invalidate (DMA → CPU sync)
        if (ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
            perror("DMA_BUF_IOCTL_SYNC START");
            break;
        }

        if (*watch_addr == WATCH_VALUE) {
            printf("find %d\n", WATCH_VALUE);
        
            // 값을 20030220으로 덮어쓰기
            *watch_addr = 20030220;
        
            // DMA와의 동기화를 위해 flush (CPU → DMA)
            sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
            ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync);
        
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
            ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync);
        
            break;
        }        

        usleep(1000);  // CPU 점유 줄이기

        // 캐시 flush 완료 (optional, RW end sync)
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
        ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync);

        // 다시 invalidate 준비
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    }

    // 6. 정리
    munmap(mapped, BUFFER_SIZE);
    close(buf_fd);
    close(heap_fd);
    return 0;
}
