/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: src/main_write_tread.c
 */
#include <inttypes.h>
#include <sys/time.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <ev.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "main.h"

#define WTH_FD_SAFETY_OFFSET 1000

#define log_info(...) \
  log("write_thread", "INFO", __VA_ARGS__)

#define log_debug(...) \
  log("write_thread", "DEBUG", __VA_ARGS__)

#define log_error(...) \
  log("write_thread", "ERROR", __VA_ARGS__)

#define WRITE_BLOCK_SIZE (1024 * 1024 /* 1MB */)
static void
sig_kill_cb(struct ev_loop *loop, ev_async *w, int revents)
{
  log_info("catch KILL signal. Exit");
  ev_break(loop, EVBREAK_ALL);
}

void *
write_thread(struct wth_context *ctx)
{
  ev_run(ctx->loop, 0);
  return NULL;
}

wth_fd wth_open(struct wth_context *ctx, char path[FH_PATH_SIZE + 1])
{
  int i;

  for (i = 0u; i < WTH_MAX_FILES; i++) {
    if (!ctx->fd[i].acquired) {
      log_debug("open(%s) -> fd#%d", path, i + WTH_FD_SAFETY_OFFSET);
      ctx->fd[i].acquired = true;
      ctx->fd[i].fd = -1;
      ctx->fd[i].expect_close = false;
      atomic_init(&ctx->fd[i].pending_to_write, 0lu);
      memcpy(ctx->fd[i].path, path, sizeof(ctx->fd[i].path));
      ev_async_send(ctx->loop, &ctx->async_open);
      return i + WTH_FD_SAFETY_OFFSET;
    }
  }
  log_error("open('%s') -> no free slots", path);
  return -1;
}

void wth_close(struct wth_context *ctx, wth_fd fd)
{
  log_debug("close request for fd#%d", fd);

  fd -= WTH_FD_SAFETY_OFFSET;
  assert(fd >= 0);
  assert(fd < WTH_MAX_FILES);

  if (ctx->fd[fd].fd != -1) {
   ctx->fd[fd].expect_close = true;
  }

  ev_async_send(ctx->loop, &ctx->async_open);
}

struct header {
  char guard_l[2]; /* must be zeros */
  unsigned idx;
  size_t data_size;
  char guard_r[2];  /* must be zeros */
};

#define HEADER_INIT {.guard_l = {'A', 'Z'}, .guard_r = {'F', 'N'}};

ssize_t wth_write(struct wth_context *ctx, wth_fd fd, uint8_t *p, size_t size)
{
  struct header hd = HEADER_INIT;

  size_t free_space;
  size_t occupied_space;
  unsigned occupied_percent;
  unsigned occupied_percent_last;

  fd -= WTH_FD_SAFETY_OFFSET;
  assert(fd >= 0);
  assert(fd < WTH_MAX_FILES);

  if (cbf_free_space(&ctx->buffer) < sizeof(hd) + size) {
    /* no free space */
    return 0;
  }

  hd.idx = fd;
  hd.data_size = size;

  pthread_mutex_lock(&ctx->write_lock);
  cbf_save(&ctx->buffer, (uint8_t*)&hd, sizeof(hd));
  cbf_save(&ctx->buffer, p, size);
  free_space = cbf_free_space(&ctx->buffer);
  occupied_space = cbf_occupied_space(&ctx->buffer);
  occupied_percent_last = ctx->occupied_percent;
  occupied_percent = (unsigned)((uint64_t)occupied_space * 100 / (uint64_t)(occupied_space + free_space));
  ctx->occupied_percent = occupied_percent;
  atomic_fetch_add(&ctx->fd[fd].pending_to_write, size);
  pthread_mutex_unlock(&ctx->write_lock);

  if (occupied_percent > 95 || occupied_percent / 10 != occupied_percent_last / 10) {
    log_debug("buffer load: %u%% (total: %"PRIuPTR", free: %"PRIuPTR")",
              occupied_percent,
              occupied_space + free_space,
              free_space);
  }

  /* decrease interrupt count */
  if (occupied_percent > 10) {
    ev_async_send(ctx->loop, &ctx->async_write);
  }
  return size;
}

static struct wth_file_desc *
open_file(struct wth_context *ctx, unsigned idx)
{
  struct wth_file_desc *fd_desc;

  assert(idx < WTH_MAX_FILES);

  fd_desc = &ctx->fd[idx];

  if (fd_desc->fd != -1)
    return fd_desc;

  log_debug("open fd#%d", idx + WTH_FD_SAFETY_OFFSET);

  fd_desc->fd = open(ctx->fd[idx].path, O_CREAT | O_TRUNC | O_WRONLY,
                                        S_IWUSR | S_IRUSR | S_IWGRP | S_IRGRP);
  if (ctx->fd[idx].fd == -1) {
    log_error("sys open(%s) fd#%d failed: %s",
              ctx->fd[idx].path, idx + WTH_FD_SAFETY_OFFSET,
              strerror(errno));
    return NULL;
  }
  else
    log_debug("open fd#%d[%d]: success", idx + WTH_FD_SAFETY_OFFSET,
                                         fd_desc->fd);

  return fd_desc;
}

static void
async_write_cb(struct ev_loop *loop, ev_async *w, int revents)
{
  uint8_t wrblk[WRITE_BLOCK_SIZE] = {0};
  struct header hd = HEADER_INIT;
  struct wth_context *ctx = ev_userdata(loop);

  struct wth_file_desc *fd_desc;

  size_t header_filled = 0u;

  while (cbf_occupied_space(&ctx->buffer) > 0) {
    size_t offset = 0u;
    size_t size;
    ssize_t written;
    /* get data */
    pthread_mutex_lock(&ctx->write_lock);

    assert(cbf_occupied_space(&ctx->buffer) > sizeof(hd));

    size = cbf_get(&ctx->buffer, wrblk, sizeof(wrblk));

    assert(size > 0u);

    cbf_discard(&ctx->buffer, size);
    pthread_mutex_unlock(&ctx->write_lock);

    while (offset != size) {
      if (header_filled != sizeof(hd)) {
        /* full or second part of scattered header */
        memcpy(((uint8_t*)&hd) + header_filled,
               wrblk + offset,
               sizeof(hd) - header_filled);
        offset += sizeof(hd) - header_filled;
        header_filled = sizeof(hd);
      }
      else if (sizeof(hd) > size - offset) {
        assert(size - offset > 0u);
        /* scattered header
         * copy first part of header
         */
        header_filled = size - offset;
        memcpy(&hd, wrblk + offset, header_filled);
        /* need more bytes */
        break;
      }

      assert(hd.guard_l[0] == 'A' &&
             hd.guard_l[1] == 'Z' &&
             hd.guard_r[0] == 'F' &&
             hd.guard_r[1] == 'N');

      fd_desc = open_file(ctx, hd.idx);
      if (!fd_desc) {
        log_error("skip frame because file not openned");
        /* skip data */
        if (hd.data_size >= size - offset) {
          hd.data_size -= (size - offset);
          /* get new data */
          break;
        }
        else {
          offset += hd.data_size;
          /* go to next header */
          continue;
        }
      }

      assert(fd_desc->acquired == true);
      assert(atomic_load(&fd_desc->pending_to_write) >= hd.data_size);

      if (hd.data_size > size - offset) {
        /* scattered copy */
        size_t write_size = size - offset;

        assert(write_size != 0);

        written = write(fd_desc->fd, wrblk + offset, write_size);
        if (written != write_size) {
          /* FIXME: what next? */
          log_error("write(fd#%d) -> written=%"PRIdPTR", expected=%"PRIuPTR": %s",
                    hd.idx, written, hd.data_size, strerror(errno));
        }
        hd.data_size -= write_size;
        atomic_fetch_sub(&fd_desc->pending_to_write, write_size);
        /* need more bytes */
        break;
      } else {
        written = write(fd_desc->fd, wrblk + offset, hd.data_size);
        if (written != hd.data_size) {
          /* FIXME: what next? */
          log_error("write(fd#%d) -> written=%"PRIdPTR", expected=%"PRIuPTR": %s",
                    hd.idx, written, hd.data_size, strerror(errno));
        }
        offset += hd.data_size;
        atomic_fetch_sub(&fd_desc->pending_to_write, hd.data_size);
        /* read next header */
        header_filled = 0u;
        continue;
      }
    }
  }
}

static void
async_open_cb(struct ev_loop *loop, ev_async *w, int revents)
{
  int i;
  struct wth_context *ctx = ev_userdata(loop);
  log_debug("async open invoked");

  for (i = 0u; i < WTH_MAX_FILES; i++) {
    if (ctx->fd[i].acquired &&
        ctx->fd[i].expect_close &&
        atomic_load(&ctx->fd[i].pending_to_write) == 0u) {
      log_debug("close fd#%d[%d]", i + WTH_FD_SAFETY_OFFSET, ctx->fd[i].fd);

      if (ctx->fd[i].fd != -1) {
        close(ctx->fd[i].fd);
        ctx->fd[i].fd = -1;
      }
      ctx->fd[i].acquired = false;
    }
  }
}

bool
write_thread_alloc(struct wth_context *ctx)
{
  size_t i = 0u;
  memset(ctx, 0, sizeof(*ctx));
  log_info("allocate write thread");
  ctx->loop = ev_loop_new(EVFLAG_AUTO);

  ev_async_init(&ctx->sig_kill, sig_kill_cb);
  ev_async_init(&ctx->async_open, async_open_cb);
  ev_async_init(&ctx->async_write, async_write_cb);

  ev_async_start(ctx->loop, &ctx->sig_kill);
  ev_async_start(ctx->loop, &ctx->async_open);
  ev_async_start(ctx->loop, &ctx->async_write);

  ev_set_userdata(ctx->loop, ctx);

  cbf_init(&ctx->buffer, 90 * 1024 * 1024); /* 90 MB */

  for (i = 0u; i < WTH_MAX_FILES; i++) {
    ctx->fd[i].fd = -1;
  }

  pthread_mutex_init(&ctx->write_lock, NULL);

  pthread_create(&ctx->thread, NULL, (void*(*)(void*))&write_thread, ctx);
  return true;
}

void
write_thread_free(struct wth_context *ctx)
{
  void *rval = NULL;
  /* send signal && wait */
  log_info("wait thread to stop...");
  ev_async_send(ctx->loop, &ctx->sig_kill);
  pthread_join(ctx->thread, &rval);

  /* free */
  ev_async_stop(ctx->loop, &ctx->async_open);
  ev_async_stop(ctx->loop, &ctx->async_write);
  ev_async_stop(ctx->loop, &ctx->sig_kill);

  pthread_mutex_destroy(&ctx->write_lock);

  ev_loop_destroy(ctx->loop);
  cbf_destroy(&ctx->buffer);
}

