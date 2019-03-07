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

#include "frame_index.h"
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

/* write target */
struct wbf {
  int fd; /* -1 and 0 is invalid fd */
  char path[FH_PATH_SIZE + 1];
  uint64_t written;
};

/* v4l buffer pointer */
struct bufinfo {
  void *p;
  /* size of allocated frame */
  size_t size;
};

/* device info */
struct devinfo {
  /* system values */
  ev_io ev;
  int fd;

  struct ev_loop *loop;
  /* preseted values */
  char path[256];

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
  struct {
    /* if limit reached, wbf.index got zero */
    size_t files_limit;
    size_t size_limit;
    uint32_t file_idx;
    struct wbf frame;
    struct wbf index;
  } trg;

  struct {
    unsigned frame_per_second;
  } cam_info;

  /* counters */
  struct {
    /* time of send STREAMON */
    struct timeval start_time;
    /* time of receive first frame after STREAMON */
    struct timeval first_frame_time;
    struct timeval first_frame_time_utc;
    struct timeval last_frame_time;
    size_t frames_arrived;
  } c;
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

static void
get_precise_time(struct timeval *tv)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;
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

/*
 * Init some options after setup device: frame_per_second, etc
 */
bool
init_device_options(struct devinfo *dev)
{
  struct v4l2_control cntr = {0};
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

  dev->cam_info.frame_per_second = fps;

  cntr.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
  cntr.value = 0;
  if (!xioctl(dev->fd, VIDIOC_S_CTRL, &cntr)) {
    fprintf(stderr, "! Exposure auto priority not disabled\n");
  } else {
    fprintf(stderr, "@ Exposure auto priority disabled\n");
  }

  return true;
}

bool
init_device(struct ev_loop *loop, struct devinfo *dev)
{
  struct v4l2_capability cap = {0};
  /* wtf?
  struct v4l2_cropcap cropcap = {0};
  struct v4l2_crop crop = {0};
  */
  struct v4l2_format fmt = {0};

  memcpy(dev->path, "/dev/video0", 12);
  dev->loop = loop;

  /* FIXME: preset configuration for camera */
#if 1
  dev->frame_width = 1280;
  dev->frame_height = 720;
#else
  dev->frame_width = 640;
  dev->frame_height = 480;
#endif
  dev->trg.size_limit = 1024 * 1024 * 128; /* limit to 128M */
  dev->trg.files_limit = 32; /* 4GB cycle */
  
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
  get_precise_time(&dev->c.start_time);

  if (!xioctl(dev->fd, VIDIOC_STREAMON, &buf.type)) {
    perror("! ioctl(VIDIOC_STREAMON)");
    return false;
  }

  fprintf(stderr,
          "* capture started at "TV_FMT"\n",
          TV_ARGS(&dev->c.start_time));

  return true;
}

static bool
wbf_write(struct devinfo *dev, struct wbf *wb, uint8_t *p, size_t len)
{
  ssize_t r;

  r = write(wb->fd, p, len);
  if (r != len) {
    fprintf(stderr, "! write to '%s' incomplete: %zd != %zu. "
            "Trying write to next file\n",
            wb->path, r, len);
    return false;
  }

  wb->written += len;
  return true;
}

static bool
make_frame_header(struct devinfo *dev)
{
  struct frame_header fh = FH_INIT_VALUE;

  fh.seq_be32 = BSWAP_BE32(dev->trg.file_idx);
  fh.fps = (uint8_t)dev->cam_info.frame_per_second;
  timebin_from_timeval(&fh.cap_time.local, &dev->c.first_frame_time);
  timebin_from_timeval(&fh.cap_time.utc, &dev->c.first_frame_time_utc);
  timebin_from_timeval(&fh.first_frame_time, &dev->c.last_frame_time);
  memcpy(fh.path, dev->trg.frame.path, sizeof(fh.path));
  return wbf_write(dev, &dev->trg.index, (uint8_t*)&fh, sizeof(fh));
}

static void
wbf_make_filename(struct devinfo *dev, struct wbf *wb, uint32_t file_no)
{
  if (wb == &dev->trg.index) {
    snprintf(wb->path, sizeof(wb->path), "idx_%010"PRIu32, file_no);
  } else if (wb == &dev->trg.frame) {
    snprintf(wb->path, sizeof(wb->path), "frm_%010"PRIu32, file_no);
  } else {
    snprintf(wb->path, sizeof(wb->path), "wtf_%010"PRIu32, file_no);
  }
}

static bool
wbf_make_file(struct devinfo *dev, struct wbf *wb)
{
  if (dev->trg.files_limit)
    wbf_make_filename(dev, wb, dev->trg.file_idx % dev->trg.files_limit);
  else
    wbf_make_filename(dev, wb, dev->trg.file_idx);

  if (wb->fd > 0)
    close(wb->fd);

  wb->fd = open(wb->path, O_CREAT | O_TRUNC | O_WRONLY,
                  S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);

  if (wb->fd == -1) {
    fprintf(stderr, "! file '%s' not openned for writing.\n", wb->path);
    return false;
  } else {
    fprintf(stderr, "@ open file '%s' writing. sequence = %"PRIu32"\n",
            wb->path, dev->trg.file_idx);
    wb->written = 0u;
  }
  return true;
}

/* generate index and frames files */
static bool
wbf_make_increment(struct devinfo *dev)
{
  if (!wbf_make_file(dev, &dev->trg.frame))
    return false;

  if (!wbf_make_file(dev, &dev->trg.index))
    return false;

  if (!make_frame_header(dev)) {
    fprintf(stderr, "! Frame header not writted: %s", strerror(errno));
    return false;
  }

  dev->trg.file_idx++;
  return true;
}

static void
capture_process(struct devinfo *dev,
                struct v4l2_buffer *cam_buf, uint8_t *p)
{
  frame_index_t fi = FI_INIT_VALUE;

  if ((dev->trg.index.written + sizeof(frame_index_t) +
       dev->trg.frame.written + cam_buf->bytesused > dev->trg.size_limit) ||
      (dev->trg.frame.fd <= 0 || dev->trg.index.fd <= 0)) {
    if (!wbf_make_increment(dev)) {
      fprintf(stderr, "! error while create new files\n");
      ev_break(dev->loop, EVBREAK_ALL);
      return;
    }
  }

  if (!wbf_write(dev, &dev->trg.frame, p, cam_buf->bytesused)) {
    fprintf(stderr, "! frame %zu not written\n", dev->c.frames_arrived);
    ev_break(dev->loop, EVBREAK_ALL);
    return;
  }

  timebin_from_timeval(&fi.tv, &cam_buf->timestamp);
  fi.offset_be64 = BSWAP_BE64(dev->trg.frame.written - cam_buf->bytesused);
  fi.size_be32 = BSWAP_BE32(cam_buf->bytesused);
  fi.seq_be64 = BSWAP_BE64((uint64_t)dev->c.frames_arrived);

  if (!wbf_write(dev, &dev->trg.index, (uint8_t*)&fi, sizeof(fi))) {
    fprintf(stderr, "! write index for frame  %zu failed\n",
            dev->c.frames_arrived);
    ev_break(dev->loop, EVBREAK_ALL);
    return;
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
  static struct timeval first_frame_in_second = {0};
  static size_t frame_counter = 0;
  struct timeval tvr = {0};
  struct timeval host_tv_cur = {0};
  struct timeval host_tvr = {0};
  static struct timeval tv = {0};
  static struct timeval host_tv = {0};
#endif

#if LOG_NOISY
  get_precise_time(&host_tv_cur);
#endif

  if (!dev->c.frames_arrived) {
    struct timeval fftv = {0};
    /* get frame time */
    get_precise_time(&dev->c.first_frame_time);
    gettimeofday(&dev->c.first_frame_time_utc, NULL);
    timersub(&dev->c.first_frame_time, &dev->c.start_time, &fftv);
    memcpy(&dev->c.last_frame_time,
           &dev->c.first_frame_time, sizeof(struct timeval));
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
    timersub(&buf.timestamp, &tv, &tvr);
    timersub(&host_tv_cur, &host_tv, &host_tvr);
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

#if LOG_NOISY
  /* get fps */
  timersub(&buf.timestamp, &first_frame_in_second, &tvr);
  if (tvr.tv_sec >= 1) {
    fprintf(stderr, "@ fps = %zu\n", dev->c.frames_arrived - frame_counter);
    frame_counter = dev->c.frames_arrived;
    memcpy(&first_frame_in_second, &buf.timestamp, sizeof(struct timeval));
  }
#endif

  capture_process(dev, &buf, dev->queue[buf.index].p);

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
  
  if (!init_device(loop, &devinfo))
    return EXIT_FAILURE;

  ev_io_init(&devinfo.ev, camera_cb, devinfo.fd, EV_READ);
  ev_io_start(loop, &devinfo.ev);

  capture(&devinfo);

  ev_run(loop, 0);

  return EXIT_SUCCESS;
}
