/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: src/main.h
 */
#ifndef _SRC_MAIN_1554547715_H_
#define _SRC_MAIN_1554547715_H_

#define log(_module, _level, _fmt, ...) \
  fprintf(stderr, "%s: [%s] " _fmt "\n", _level, _module, ##__VA_ARGS__)

#define WTH_MAX_FILES 16

#include "frame_index.h"
#include "circle_buffer.h"

struct wth_file_desc {
  int fd;
  char path[FH_PATH_SIZE + 1];
  bool acquired;
  size_t pending_to_write;
  bool expect_close;
};

struct wth_context {
  struct ev_loop *loop;
  struct ev_async sig_kill;

  struct ev_async async_open;
  struct ev_async async_write;

  struct wth_file_desc fd[WTH_MAX_FILES];

  struct circle_buffer buffer;

  pthread_t thread;
};

extern bool write_thread_alloc(struct wth_context *ctx);
extern void write_thread_free(struct wth_context *ctx);

typedef int wth_fd;
/* open file for writing, return fd */
extern wth_fd wth_open(struct wth_context *ctx, char path[FH_PATH_SIZE + 1]);
extern ssize_t wth_write(struct wth_context *ctx, wth_fd fd, uint8_t *p, size_t size);
extern void wth_close(struct wth_context *ctx, wth_fd fd);

#endif /* _SRC_MAIN_1554547715_H_ */

