/* vim: ft=c ff=unix fenc=utf-8
 * file: main.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <strings.h>
#include <sys/ioctl.h>

#include <ev.h>

#include <linux/videodev2.h>

#define LOG_NOISY 0

#ifndef MAX
# define MAX(a,b) (((a)>(b))?(a):(b))
#endif
#ifndef MIN
# define MIN(a,b) (((a)<(b))?(a):(b))
#endif

#define WQUEUE_WRITE_BLOCK_SZ 4098
#define BUFFERS_WRITE_COUNT 3
#if 1
# define BUFFERS_SWAP_COUNT 8
#else
# define BUFFERS_SWAP_COUNT VIDEO_MAX_FRAME
#endif

struct partinfo {
  ev_io ev;
  bool ev_started;
  uint8_t *p;
  /* descriptor of file to write */
  int fd;
  /* maximum size of each stored frame */
  size_t frame_max_size;
  /* maximum count of stored frames */
  size_t frame_capacity;
  /* count and size of stored frames to buffer */
  size_t frame_stored;
  size_t frame_stored_size;
  /* writed bytes to file */
  size_t writed_to_disk;
};

struct bufinfo {
  void *p;
  /* size of allocated frame */
  size_t size;
  /* size of captured frame */
  size_t filled;
};

struct devinfo {
  /* system values */
  ev_io ev;
  int fd;
 
  /* preseted values */
  char path[256];

  /* approximately maximum size of compressed frame */
  size_t frame_size_comp;
  /* size of uncompressed frame */
  size_t frame_size;

  size_t frame_per_second;
  size_t frame_width;
  size_t frame_height;

  /* calculated values */
  /* input queue */
  struct bufinfo *queue;
  size_t queue_size;  
  size_t queued; /* count of queued buffers */
  /* output queue */
  struct partinfo *wqueue;
  size_t wqueue_idx;
  size_t wqueue_size;
} devinfo;

static inline int
xioctl(int fh, unsigned long int request, void *arg)
{
  int r = 0;

  do {
    if (r) {
      fprintf(stderr, "! ioctl got errno=EINTR\n");
    }
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);

  if (r == -1)
    return false;

  return true;
}

void
deinit_device(struct devinfo *dev)
{
  fprintf(stderr, "* close cam: %s\n", dev->path);
  close(dev->fd);
  /* TODO: free queue buffer */
  if (dev->queue) {
    if (dev->queue[0].p) {
      free(dev->queue[0].p);
    }
    free(dev->queue);
  }
  if (dev->wqueue) {
    free(dev->wqueue);
  }
}

void
atexit_cb()
{
    deinit_device(&devinfo);
}

static bool
init_device_rqueue_alloc(struct devinfo *dev)
{
  size_t i = 0u;
  dev->queue_size = BUFFERS_SWAP_COUNT;

  /* allocate memory for buffers */
  dev->queue = calloc(dev->queue_size, sizeof(*dev->queue));
  if (!dev->queue) {
    fprintf(stderr, "! out of memory while allocating frame buffers\n");
    return false;
  }

  dev->queue[0].p = calloc(dev->queue_size, dev->frame_size);
  if (!dev->queue[0].p) {
    fprintf(stderr, "! out of memory while allocating frame buffers\n");
    return false;
  }

  for (i = 0; i < dev->queue_size; i++) {
    dev->queue[i].size = dev->frame_size;
    dev->queue[i].p = (char*)dev->queue[0].p + dev->frame_size * i;
  }

  fprintf(stderr, "@ summary capture mem: %zu buffers, "
                  "memory buffers: %zu bytes, "
                  "memory queue: %zu bytes\n",
          dev->queue_size,
          dev->queue_size * dev->frame_size,
          dev->queue_size * sizeof(*dev->queue));
  return true;
}

static bool
init_device_wqueue_alloc(struct devinfo *dev)
{
  size_t capsize = 0u;
  uint8_t *ptr = NULL;

  capsize = dev->frame_size_comp * dev->frame_per_second;

  dev->wqueue_size = BUFFERS_WRITE_COUNT;
  dev->wqueue = calloc(dev->wqueue_size, sizeof(*dev->wqueue) + capsize);
  if (!dev->wqueue) {
    fprintf(stderr, "!  of memory while allocating write buffers: "
                    "required %zu bytes\n",
                    (capsize + sizeof(*dev->wqueue)) * dev->wqueue_size);
    return false;
  }

  ptr = (void*)(dev->wqueue + dev->wqueue_size);

  for (size_t i = 0u; i < dev->wqueue_size; i++) {
    dev->wqueue[i].p = ptr + capsize * i;
    dev->wqueue[i].frame_max_size = dev->frame_size_comp;
    dev->wqueue[i].frame_capacity = dev->frame_per_second;
    fprintf(stderr, "@ assign write buffer #%zu: %p, "
                    "size: %zu, "
                    "approx frame size: %zu, "
                    "frame capcity: %zu\n",
                    i,
                    dev->wqueue[i].p,
                    capsize,
                    dev->wqueue[i].frame_max_size,
                    dev->wqueue[i].frame_capacity);
  }

  fprintf(stderr, "@ summary write mem: %zu buffers, "
                  "memory buffers: %zu bytes, "
                  "memory queue: %zu bytes\n",
                  dev->wqueue_size,
                  capsize * dev->wqueue_size,
                  sizeof(*dev->wqueue) * dev->wqueue_size);

  return true;
}

bool
init_device(struct devinfo *dev)
{
  struct v4l2_capability cap = {0};
  /* wtf?
  struct v4l2_cropcap cropcap = {0};
  struct v4l2_crop crop = {0};
  */
  struct v4l2_format fmt = {0};

  memcpy(dev->path, "/dev/video1", 12);

  /* FIXME: preset configuration for camera */
#if 1
  dev->frame_per_second = 120;
  dev->frame_width = 1280;
  dev->frame_height = 720;
  dev->frame_size = 1843200;
  dev->frame_size_comp = 250000;
#else
  dev->frame_per_second = 240;
  dev->frame_width = 640;
  dev->frame_height = 480;
  def->frame_size = 614400;
  dev->frame_size_comp = 150000;
#endif
  
  fprintf(stderr, "* open cam: %s\n", dev->path);
  dev->fd = open(dev->path, O_RDWR | O_NONBLOCK, 0);
  if (dev->fd == -1) {
    perror("! open");
    free(dev->queue);
    return false;
  }

  atexit(atexit_cb);

  /* query capabilities */
  if (!xioctl(dev->fd, VIDIOC_QUERYCAP, &cap)) {
    if (errno == EINVAL) {
      fprintf(stderr, "! invalid device: %s\n", dev->path);
    } else {
      perror("! ioctl(VIDIOC_QUERYCAP)");
    }

    return false;
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "! Device is not support capturing: %s\n", dev->path);
    return false;
  }

  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "! Device not support streaming: %s\n", dev->path);
    return false;
  }

  /* set frame format */
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = dev->frame_width;
  fmt.fmt.pix.height      = dev->frame_height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

  if (!xioctl(dev->fd, VIDIOC_S_FMT, &fmt)) {
    fprintf(stderr, "! ioctl(VIDIOC_S_FMT) failed: %d\n", errno);
    return false;
  }

  if (dev->frame_size != fmt.fmt.pix.sizeimage) {
      fprintf(stderr, "! invalid presetted value `frame_size`: %zu != %u",
              dev->frame_size, fmt.fmt.pix.sizeimage);
      return false;
  }

  if (dev->frame_size_comp > fmt.fmt.pix.sizeimage) {
      fprintf(stderr, "! invalid presetted value `frame_size_comp`: %zu > %u",
              dev->frame_size_comp, fmt.fmt.pix.sizeimage);
      return false;
  }

  fprintf(stderr, "@ camera options:\n");
  fprintf(stderr, "@  image: %"PRIu32"x%"PRIu32"\n",
          fmt.fmt.pix.width,
          fmt.fmt.pix.height);
  fprintf(stderr, "@  size : %"PRIu32"\n", fmt.fmt.pix.sizeimage);
  fprintf(stderr, "@  flags: 0x%08"PRIx32"\n", fmt.fmt.pix.flags);

  fprintf(stderr, "* camera opened\n");

  if (!init_device_rqueue_alloc(dev))
    return false;

  if (!init_device_wqueue_alloc(dev))
    return false;

  return true;
}

bool
capture(struct devinfo *dev)
{
  struct v4l2_buffer buf = {0};
	struct v4l2_requestbuffers req = {0};
  size_t i;

  req.count = dev->queue_size;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

  if (!xioctl(dev->fd, VIDIOC_REQBUFS, &req)) {
    if (errno == EINVAL) {
      fprintf(stderr, "! Device not support userp i/o\n");
    } else {
      perror("! ioctl(VIDIOC_REQBUFS)");
    }
    return false;
  }

  for (i = 0u; i < dev->queue_size; i++) {
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_USERPTR;
    buf.index = i;
    buf.m.userptr = (unsigned long)dev->queue[i].p;
    buf.length = dev->queue[i].size;
    fprintf(stderr,
            "@ queue buffer %"PRIu32": ptr=%p, size=%"PRIu32"\n",
            buf.index, (void*)buf.m.userptr, buf.length);

    if (!xioctl(dev->fd, VIDIOC_QBUF, &buf)) {
      fprintf(stderr, "! error while queue buffer %zu: %s\n",
              i, strerror(errno));
      return false;
    } else {
      dev->queued++;
    }
  }

  /* start capture */
  if (!xioctl(dev->fd, VIDIOC_STREAMON, &buf.type)) {
    perror("! ioctl(VIDIOC_STREAMON)");
    return false;
  }

  fprintf(stderr, "* capture started\n");

  return true;
}

static struct partinfo *
wqueue_get_free(struct devinfo *dev)
{
  static size_t fail_call_counter = 0u;
  struct partinfo *wq = NULL;
  size_t i = dev->wqueue_idx;

  do {
      wq = &dev->wqueue[dev->wqueue_idx];
      if (wq->frame_capacity > wq->frame_stored) {
        dev->wqueue_idx = i;

        if (fail_call_counter)
          fprintf(stderr, "@ resume writing frames. lost count: %zu\n",
                          fail_call_counter);

        fail_call_counter = 0u;
        return wq;
      }
  } while ((i = ((i + 1) % dev->wqueue_size)) != dev->wqueue_idx);

  if (!fail_call_counter)
    fprintf(stderr, "! no free write buffer. frame dropped\n");

  fail_call_counter++;
  return NULL;
}

static void
wqueue_cb(struct ev_loop *loop, ev_io *w, int revents)
{
  struct partinfo *pinfo = (struct partinfo*)w;
  size_t write_len = WQUEUE_WRITE_BLOCK_SZ;
  int r = 0;

  if (pinfo->writed_to_disk + write_len > pinfo->frame_stored_size) {
    write_len = pinfo->frame_stored_size - pinfo->writed_to_disk;
  }

  if (write_len == 0u) {
    fprintf(stderr, "! write_len == 0. WTF!?\n");
  } else {
      r = write(pinfo->fd, pinfo->p + pinfo->writed_to_disk, write_len);
  }

  if (r == -1) {
    fprintf(stderr, "! write(%zu) failed: %s\n", write_len, strerror(errno));
  }

  pinfo->writed_to_disk += r;

  if (pinfo->writed_to_disk == pinfo->frame_stored_size) {
    ev_io_stop(loop, &pinfo->ev);
    if (pinfo->frame_capacity == pinfo->frame_stored) {
      close(pinfo->fd);
      pinfo->fd = 0;
      pinfo->frame_stored = 0;
      pinfo->frame_stored_size = 0;
      pinfo->writed_to_disk = 0;
#if LOG_NOISY
      fprintf(stderr, "@ free write buffer\n");
#endif
    }
  }
}

void
capture_process(struct ev_loop *loop, struct devinfo *dev, struct bufinfo *buf)
{
  struct partinfo *pinfo = wqueue_get_free(dev);

  if (!pinfo)
    return;

  if (buf->filled > pinfo->frame_max_size) {
    fprintf(stderr, "! frame to big: %zu > %zu. dropped\n",
                    buf->filled,
                    pinfo->frame_max_size);
    /* TODO: copy black frame */
  }
  else {
    memcpy(pinfo->p + pinfo->frame_stored_size, buf->p, buf->filled);
    pinfo->frame_stored_size += buf->filled;
    if (!pinfo->ev_started) {
      if (!pinfo->fd) {
        char path[256] = {0};
        snprintf(path, sizeof(path) - 1, "%zu.mjpeg", time(NULL));
        pinfo->fd = open(path, O_CREAT | O_RDWR | O_NONBLOCK,
                               S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);
        if (pinfo->fd == -1) {
          fprintf(stderr, "! open(%s) failed. frame dropped: %s\n",
                          path, strerror(errno));

        }
      }
      ev_io_init(&pinfo->ev, wqueue_cb, pinfo->fd, EV_WRITE);
      ev_io_start(loop, &pinfo->ev);
    }
  }
  pinfo->frame_stored++;
}

static void
camera_cb(struct ev_loop *loop, ev_io *w, int revents)
{
  struct v4l2_buffer buf = {0};
  struct devinfo *dev = (struct devinfo*)w;
  struct timeval tvr = {0};
  struct timeval host_tv_cur = {0};
  struct timeval host_tvr = {0};
  static struct timeval tv = {0};
  static struct timeval host_tv = {0};
  
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_USERPTR;

  gettimeofday(&host_tv_cur, NULL);

  if (!xioctl(dev->fd, VIDIOC_DQBUF, &buf)) {
    fprintf(stderr, "! ioctl(VIDIOC_DQBUF) failed: %s\n", strerror(errno));
  } else {
    dev->queued--;
    timersub(&buf.timestamp, &tv, &tvr);
    timersub(&host_tv_cur, &host_tv, &host_tvr);
    memcpy(&tv, &buf.timestamp, sizeof(tv));
    memcpy(&host_tv, &host_tv_cur, sizeof(host_tv));
    dev->queue[buf.index].filled = buf.bytesused;
#if LOG_NOISY
    fprintf(stderr,
            "@ buf: index=%"PRIu32", "
            "bytesused=%"PRIu32", "
            "flags=0x%08"PRIx32", "
            "sequence=%"PRIu32", "
            "queued=%zu, "
            "%ld.%06ld, from last: %ld.%06ld, "
            "host last: %ld.%06ld"
            "\n",
            buf.index, buf.bytesused, buf.flags, buf.sequence,
            dev->queued,
            buf.timestamp.tv_sec, buf.timestamp.tv_usec,
            tvr.tv_sec, tvr.tv_usec,
            host_tvr.tv_sec, host_tvr.tv_usec);
#endif
  }

  capture_process(loop, dev, &dev->queue[buf.index]);

  if (!xioctl(dev->fd, VIDIOC_QBUF, &buf)) {
    fprintf(stderr, "! error while queue buffer %"PRIu32": %s\n",
            buf.index, strerror(errno));
  } else {
    dev->queued++;
  }

  if (!dev->queued) {
    fprintf(stderr, "! queue empty");
    ev_break(loop, EVBREAK_ALL);
  }
}


int
main(int argc, char *argv[])
{
  struct ev_loop *loop = EV_DEFAULT;
  
  init_device(&devinfo);

  ev_io_init(&devinfo.ev, camera_cb, devinfo.fd, EV_READ);
  ev_io_start(loop, &devinfo.ev);

  capture(&devinfo);

  ev_run(loop, 0);

  return EXIT_SUCCESS;
}
