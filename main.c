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
#include <sys/ioctl.h>

#include <ev.h>

#include <linux/videodev2.h>

#if 1
# define BUFFERS_SWAP_COUNT 8
#else
# define BUFFERS_SWAP_COUNT VIDEO_MAX_FRAME
#endif

struct bufinfo {
  void *p;
  size_t size;
  size_t lenght;
};

struct devinfo {
  /* system values */
  ev_io ev;
  int fd;
 
  /* preseted values */
  char path[256];

  size_t frame_rate;
  size_t frame_width;
  size_t frame_height;

  /* calculated values */
  size_t frame_size;
  struct bufinfo *queue;
  size_t queue_size;  
  size_t queued; /* count of queued buffers */
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
  free(dev->queue);
}

void
atexit_cb()
{
    deinit_device(&devinfo);
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
  size_t i = 0u;

  memcpy(dev->path, "/dev/video1", 12);

  /* FIXME: preset configuration for camera */
#if 1
  dev->frame_rate = 120;
  dev->frame_width = 1280;
  dev->frame_height = 720;
#else
  dev->frame_rate = 240;
  dev->frame_width = 640;
  dev->frame_height = 480;
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

  dev->frame_size = fmt.fmt.pix.sizeimage;

  fprintf(stderr, "@ camera options:\n");
  fprintf(stderr, "@  image: %"PRIu32"x%"PRIu32"\n",
          fmt.fmt.pix.width,
          fmt.fmt.pix.height);
  fprintf(stderr, "@  size : %"PRIu32"\n", fmt.fmt.pix.sizeimage);
  fprintf(stderr, "@  flags: 0x%08"PRIx32"\n", fmt.fmt.pix.flags);

  fprintf(stderr, "* camera opened\n");

  /* allocate memory for buffers */
  dev->frame_size = fmt.fmt.pix.sizeimage;
  dev->queue_size = BUFFERS_SWAP_COUNT;
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
    dev->queue[i].p = dev->queue[0].p + dev->frame_size * i;
  }

  fprintf(stderr, "@ summary capture mem: %zu buffers, "
                  "memory buffers: %zu bytes, "
                  "memory queue: %zu bytes\n",
          dev->queue_size,
          dev->queue_size * dev->frame_size,
          dev->queue_size * sizeof(*dev->queue));

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

static void
camera_cb(struct ev_loop *loop, ev_io *w, int revents)
{
  struct v4l2_buffer buf = {0};
  struct devinfo *dev = (struct devinfo*)w;
  struct timeval tvr = {0};
  static struct timeval tv = {0};
  
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_USERPTR;

  if (!xioctl(dev->fd, VIDIOC_DQBUF, &buf)) {
    fprintf(stderr, "! ioctl(VIDIOC_DQBUF) failed: %s\n", strerror(errno));
  } else {
    dev->queued--;
    timersub(&buf.timestamp, &tv, &tvr);
    memcpy(&tv, &buf.timestamp, sizeof(tv));
    dev->queue[buf.index].lenght = buf.bytesused;
    fprintf(stderr,
            "@ buf: index=%"PRIu32", "
            "bytesused=%"PRIu32", "
            "flags=0x%08"PRIx32", "
            "sequence=%"PRIu32", "
            "queued=%zu, "
            "%ld.%06ld, from last: %ld.%06ld\n",
            buf.index, buf.bytesused, buf.flags, buf.sequence,
            dev->queued,
            buf.timestamp.tv_sec, buf.timestamp.tv_usec,
            tvr.tv_sec, tvr.tv_usec);
  }

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
