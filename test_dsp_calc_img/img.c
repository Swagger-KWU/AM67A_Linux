#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <string.h>
#include <errno.h>

#ifndef DMA_HEAP_IOCTL_ALLOC
#include <linux/types.h>
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u32 heap_flags;
    __u32 reserved;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif

#define DMA_HEAP_NAME "/dev/dma_heap/carveout_ai-share-memory@a7000000"
#define RGB_SIZE   (512 * 512 * 3)
#define GRAY_SIZE  (512 * 512)
#define BUFFER_SIZE (4096 + RGB_SIZE + GRAY_SIZE)

int main() {
    int heap_fd = -1, buf_fd = -1;
    void *mapped = NULL;
    struct dma_heap_allocation_data alloc_data = {
        .len = BUFFER_SIZE,
        .fd_flags = O_RDWR,
        .heap_flags = 0,
    };

    // 1. dma_heap open
    heap_fd = open(DMA_HEAP_NAME, O_RDWR);
    if (heap_fd < 0) {
        perror("open dma_heap");
        return 1;
    }

    // 2. 버퍼 할당
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return 1;
    }
    buf_fd = alloc_data.fd;

    // 3. mmap
    mapped = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        close(buf_fd);
        close(heap_fd);
        return 1;
    }

    // 4. 이미지 불러오기
    int width, height, channels;
    uint8_t* img = stbi_load("Lenna.png", &width, &height, &channels, 3); // force grayscale
    if (!img) {
        fprintf(stderr, "Failed to load image\n");
        munmap(mapped, BUFFER_SIZE);
        close(buf_fd);
        close(heap_fd);
        return 1;
    }

    printf("Loaded image %dx%d\n", width, height);
    if (width != 512 || height != 512) {
        fprintf(stderr, "Image must be 512x512\n");
        stbi_image_free(img);
        munmap(mapped, BUFFER_SIZE);
        close(buf_fd);
        close(heap_fd);
        return 1;
    }

    // 5. DMA_BUF_SYNC_START
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
    ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync);

    // 6. 0xA7000001 위치에 이미지 복사 (offset = 1)
    memcpy((uint8_t*)mapped + 1, img, RGB_SIZE);

    // 7. 0xA7000000에 32 저장
    *((volatile uint8_t*)mapped) = 32;

    printf("Image written to DMA buffer\n");

    while(*((volatile uint8_t*)mapped) != 64) {
    	usleep(1000);
    }
    
    printf("GreyScale Complete\n");
    
    uint8_t* result_ptr = (uint8_t*)mapped + 1 + RGB_SIZE;
    if (!stbi_write_png("output.png", 512, 512, 1, result_ptr, 512)) {
        fprintf(stderr, "Failed to save output.png\n");
    } else {
        printf("Saved grayscale image as output.png\n");
    }

    // 8. DMA_BUF_SYNC_END
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync);

    // 9. 정리
    stbi_image_free(img);
    munmap(mapped, BUFFER_SIZE);
    close(buf_fd);
    close(heap_fd);

    return 0;
}

