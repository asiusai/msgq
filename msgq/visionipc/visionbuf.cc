#include "msgq/visionipc/visionbuf.h"

#include <atomic>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

#ifndef __APPLE__
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#endif

std::atomic<int> offset = 0;

static void *malloc_with_fd(size_t len, int *fd) {
  char full_path[0x100];

#ifdef __APPLE__
  snprintf(full_path, sizeof(full_path)-1, "/tmp/visionbuf_%d_%d", getpid(), offset++);
#else
  snprintf(full_path, sizeof(full_path)-1, "/dev/shm/msgq_visionbuf_%d_%d", getpid(), offset++);
#endif

  *fd = open(full_path, O_RDWR | O_CREAT, 0664);
  assert(*fd >= 0);

  unlink(full_path);

  int ret = ftruncate(*fd, len);
  assert(ret == 0);
  void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
  assert(addr != MAP_FAILED);

  return addr;
}

#ifndef __APPLE__
static void *dma_heap_alloc(size_t len, int *fd) {
  int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
  if (heap_fd < 0) return MAP_FAILED;

  struct dma_heap_allocation_data allocation = {};
  allocation.len = len;
  allocation.fd_flags = O_RDWR | O_CLOEXEC;
  int ret;
  do {
    ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation);
  } while (ret < 0 && errno == EINTR);
  close(heap_fd);
  if (ret != 0) return MAP_FAILED;

  void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, allocation.fd, 0);
  if (addr == MAP_FAILED) {
    close(allocation.fd);
    return MAP_FAILED;
  }

  *fd = allocation.fd;
  return addr;
}

static int sync_dma_buffer(int fd, uint64_t flags) {
  struct dma_buf_sync sync = {.flags = flags};
  int ret;
  do {
    ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
  } while (ret < 0 && errno == EINTR);
  return (ret == 0 || errno == ENOTTY) ? 0 : ret;
}
#endif

void VisionBuf::allocate(size_t length) {
  this->len = length;
  this->mmap_len = this->len + sizeof(uint64_t);
#ifndef __APPLE__
  this->addr = dma_heap_alloc(this->mmap_len, &this->fd);
  if (this->addr != MAP_FAILED) this->handle = -1;
#endif
  if (this->addr == nullptr || this->addr == MAP_FAILED) {
    this->addr = malloc_with_fd(this->mmap_len, &this->fd);
  }
  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::import(){
  assert(this->fd >= 0);
  this->addr = mmap(NULL, this->mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);
  assert(this->addr != MAP_FAILED);

  this->frame_id = (uint64_t*)((uint8_t*)this->addr + this->len);
}

void VisionBuf::init_yuv(size_t init_width, size_t init_height, size_t init_stride, size_t init_uv_offset){
  this->width = init_width;
  this->height = init_height;
  this->stride = init_stride;
  this->uv_offset = init_uv_offset;

  this->y = (uint8_t *)this->addr;
  this->uv = this->y + this->uv_offset;
}

int VisionBuf::sync(int dir) {
#ifndef __APPLE__
  assert(dir == VISIONBUF_SYNC_FROM_DEVICE || dir == VISIONBUF_SYNC_TO_DEVICE);
  uint64_t access = dir == VISIONBUF_SYNC_FROM_DEVICE ? DMA_BUF_SYNC_READ : DMA_BUF_SYNC_WRITE;
  int ret = sync_dma_buffer(this->fd, DMA_BUF_SYNC_START | access);
  return ret == 0 ? sync_dma_buffer(this->fd, DMA_BUF_SYNC_END | access) : ret;
#endif
  return 0;
}

int VisionBuf::free() {
  int err = munmap(this->addr, this->mmap_len);
  if (err != 0) return err;

  err = close(this->fd);
  return err;
}

uint64_t VisionBuf::get_frame_id() {
  return *frame_id;
}

void VisionBuf::set_frame_id(uint64_t id) {
  *frame_id = id;
}
