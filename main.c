/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
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
#include <sys/time.h>

#include <ev.h>

#include <linux/videodev2.h>

#include "circle_buffer.h"

#define LOG_NOISY 0
#define FRAMES_DB "frames.mjpeg"
#define INDEX_DB "frames_idx.db"

#ifndef MAX
# define MAX(a,b) (((a)>(b))?(a):(b))
#endif
#ifndef MIN
# define MIN(a,b) (((a)<(b))?(a):(b))
#endif

#define WQUEUE_WRITE_BLOCK_SZ 4098
#if 1
# define BUFFERS_SWAP_COUNT 8
#else
# define BUFFERS_SWAP_COUNT VIDEO_MAX_FRAME
#endif

#define timespecfix(_res)                 \
  do {                                    \
    if ((_res)->tv_nsec < 0) {            \
      (_res)->tv_sec--;                   \
      (_res)->tv_nsec += 1000000000;      \
    }                                     \
    if ((_res)->tv_nsec > 1000000000) {    \
      (_res)->tv_sec++;                   \
      (_res)->tv_nsec -= 1000000000;      \
    }                                     \
  } while (0)

#define timespecsub(_a, _b, _res)                     \
  do {                                                \
    (_res)->tv_sec = (_a)->tv_sec - (_b)->tv_sec;     \
    (_res)->tv_nsec = (_a)->tv_nsec - (_b)->tv_nsec;  \
    timespecfix(_res);                                \
  } while (0)

#define timespecadd(_a, _b, _res)                     \
  do {                                                \
    (_res)->tv_sec = (_a)->tv_sec + (_b)->tv_sec;     \
    (_res)->tv_nsec = (_a)->tv_nsec + (_b)->tv_nsec;  \
    timespecfix(_res);                                \
  } while (0)


struct wbuf {
  ev_io ev;
  int fd;
  struct circle_buffer cbf;
};

struct bufinfo {
  void *p;
  /* size of allocated frame */
  size_t size;
};

struct devinfo {
  /* system values */
  ev_io ev;
  int fd;
 
  /* preseted values */
  char path[256];

  size_t disk_frame_buffer_size;
  size_t disk_index_buffer_size;

  /* size of uncompressed frame */
  size_t frame_size;

  size_t frame_width;
  size_t frame_height;

  /* calculated values */
  /* input queue */
  struct bufinfo *queue;
  size_t queue_size;  
  size_t queued; /* count of queued buffers */

  /* output queue: frames and indexes */
  struct wbuf wqueue_frame;
  struct wbuf wqueue_index;

  /* counters */
  struct {
    /* time of send STREAMON */
    struct timespec start_time;
    /* time of receive first frame after STREAMON */
    struct timespec first_frame_time;
    size_t frames_arrived;
  } c;
} devinfo;

#define TV_FMT "%zu.%.09ld"
#define TV_ARGS(_tv) (_tv)->tv_sec, (_tv)->tv_nsec

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
  /* TODO: free circle_buffer */
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
init_device_wqueue_alloc(struct devinfo *dev,
                         struct wbuf *wbuf, char *path, size_t size)
{
  if (!cbf_init(&wbuf->cbf, size)) {
    fprintf(stderr, "! cannot alloc %zu bytes for '%s': %s\n",
            size, path, strerror(errno));
    return false;
  }

  wbuf->fd = open(path,
                  O_CREAT | O_WRONLY | O_NONBLOCK | O_TRUNC,
                  S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);

  if (wbuf->fd == -1) {
    fprintf(stderr, "! open db %s failed: %s\n", path, strerror(errno));
    return false;
  }

  return true;
}

/*
 * Init some options after setup device: frame_per_second, etc
 */
bool
init_device_options(struct devinfo *dev)
{
  struct v4l2_streamparm parm = {0};
  size_t fps = 0u;
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (!xioctl(dev->fd, VIDIOC_G_PARM, &parm)) {
    return false;
  }

  if (!(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
    fprintf(stderr, "! Driver not supported timeperframe feature\n");
    return false;
  }

  if (!parm.parm.capture.timeperframe.denominator ||
      !parm.parm.capture.timeperframe.numerator) {
      fprintf(stderr,
              "! Invalid frame per seconds data: denominator=%u, numenator=%u\n",
              parm.parm.capture.timeperframe.denominator,
              parm.parm.capture.timeperframe.numerator);
      return false;
  }

  fps = (size_t)(parm.parm.capture.timeperframe.denominator /
                 parm.parm.capture.timeperframe.numerator);

  fprintf(stderr, "@ frame per seconds: %zu\n", fps);
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

  memcpy(dev->path, "/dev/video0", 12);

  /* FIXME: preset configuration for camera */
#if 1
  dev->frame_width = 1280;
  dev->frame_height = 720;
#else
  dev->frame_width = 640;
  dev->frame_height = 480;
#endif
  dev->disk_frame_buffer_size = 90000000; /* 90 MiB */
  dev->disk_index_buffer_size = 1048576; /* 1MB */
  
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
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
  fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

  if (!xioctl(dev->fd, VIDIOC_S_FMT, &fmt)) {
    fprintf(stderr, "! ioctl(VIDIOC_S_FMT) failed: %d\n", errno);
    return false;
  }

  dev->frame_size = fmt.fmt.pix.sizeimage;

  fprintf(stderr, "@ image format options:\n");
  fprintf(stderr, "@  image: %"PRIu32"x%"PRIu32"\n",
          fmt.fmt.pix.width,
          fmt.fmt.pix.height);
  fprintf(stderr, "@  size : %"PRIu32"\n", fmt.fmt.pix.sizeimage);
  fprintf(stderr, "@  flags: 0x%08"PRIx32"\n", fmt.fmt.pix.flags);
  fprintf(stderr, "@  pixel format: '%.4s'\n", (char*)&fmt.fmt.pix.pixelformat);

  if (!init_device_options(dev))
    return false;

  fprintf(stderr, "* camera opened\n");

  if (!init_device_rqueue_alloc(dev))
    return false;

  if (!init_device_wqueue_alloc(dev, &dev->wqueue_frame,
                                FRAMES_DB, dev->disk_frame_buffer_size))
    return false;

  if (!init_device_wqueue_alloc(dev, &dev->wqueue_index,
                                INDEX_DB, dev->disk_index_buffer_size))
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
  memset(&dev->c, 0, sizeof(dev->c));
  clock_gettime(CLOCK_MONOTONIC, &dev->c.start_time);

  if (!xioctl(dev->fd, VIDIOC_STREAMON, &buf.type)) {
    perror("! ioctl(VIDIOC_STREAMON)");
    return false;
  }

  fprintf(stderr,
          "* capture started at "TV_FMT"\n",
          TV_ARGS(&dev->c.start_time));

  return true;
}

/* write cb for buffer */
static void
wqueue_frames_cb(struct ev_loop *loop, ev_io *w, int revents)
{
  struct wbuf *wb = (struct wbuf*)w;
  uint8_t p[WQUEUE_WRITE_BLOCK_SZ] = {0};
  size_t len = WQUEUE_WRITE_BLOCK_SZ;
  ssize_t wlen = 0;

  len = cbf_get(&wb->cbf, p, len);
  if (!len) {
    /* stop event when buffer is empty */
    ev_io_stop(loop, w);
    return;
  }

  wlen = write(wb->fd, p, len);
  if (wlen <= 0) {
    fprintf(stderr,
            "! error write %zu bytes to fd %d: %s\n",
            len, wb->fd, strerror(errno));
    return;
  }

  cbf_discard(&wb->cbf, wlen);
  /* TODO: write index */
}

void
capture_process(struct ev_loop *loop, struct devinfo *dev,
                struct v4l2_buffer *cam_buf, uint8_t *p)
{
  bool need_started = cbf_is_empty(&dev->wqueue_frame.cbf);

  if (!cbf_save(&dev->wqueue_frame.cbf, p, cam_buf->bytesused)) {
    fprintf(stderr, "! buffer has no free space. Frame dropped\n");
    exit(1);
    return;
  }

  /* TODO: need_started */
  if (need_started) {
    ev_io_init(&dev->wqueue_frame.ev,
               wqueue_frames_cb, dev->wqueue_frame.fd, EV_WRITE);
    ev_io_start(loop, &dev->wqueue_frame.ev);
  }
}

static void
camera_cb(struct ev_loop *loop, ev_io *w, int revents)
{
  struct v4l2_buffer buf = {
                            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                            .memory = V4L2_MEMORY_USERPTR
                           };
  struct devinfo *dev = (struct devinfo*)w;
#if LOG_NOISY
  struct timespec tvr = {0};
  struct timespec host_tv_cur = {0};
  struct timespec host_tvr = {0};
  static struct timespec tv = {0};
  static struct timespec host_tv = {0};
#endif

#if LOG_NOISY
  clock_gettime(CLOCK_MONOTONIC, &host_tv_cur);
#endif

  if (!dev->c.frames_arrived) {
    struct timespec fftv = {0};
    clock_gettime(CLOCK_MONOTONIC, &dev->c.first_frame_time);
    timespecsub(&dev->c.first_frame_time, &dev->c.start_time, &fftv);
    /* update information about first frame */
    fprintf(stderr,
            "@ first frame arrived in: "TV_FMT" seconds\n",
            TV_ARGS(&fftv));
  }

  dev->c.frames_arrived++;

  if (!xioctl(dev->fd, VIDIOC_DQBUF, &buf)) {
    fprintf(stderr, "! ioctl(VIDIOC_DQBUF) failed: %s\n", strerror(errno));
  } else {
    dev->queued--;
#if LOG_NOISY
    timespecsub(&buf.timestamp, &tv, &tvr);
    timespecsub(&host_tv_cur, &host_tv, &host_tvr);
    memcpy(&tv, &buf.timestamp, sizeof(tv));
    memcpy(&host_tv, &host_tv_cur, sizeof(host_tv));
    fprintf(stderr,
            "@ buf: index=%"PRIu32", "
            "bytesused=%"PRIu32", "
            "flags=0x%08"PRIx32", "
            "sequence=%"PRIu32", "
            "queued=%zu, "
            TV_FMT", from last: "TV_FMT", "
            "host last: " TV_FMT
            "\n",
            buf.index, buf.bytesused, buf.flags, buf.sequence,
            dev->queued,
            TV_ARGS(&buf.timestamp),
            TV_ARGS(&tvr),
            TV_ARGS(&host_tvr));
#endif
  }

  capture_process(loop, dev, &buf, dev->queue[buf.index].p);

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
  
  if (!init_device(&devinfo))
    return EXIT_FAILURE;

  ev_io_init(&devinfo.ev, camera_cb, devinfo.fd, EV_READ);
  ev_io_start(loop, &devinfo.ev);

  capture(&devinfo);

  ev_run(loop, 0);

  return EXIT_SUCCESS;
}
