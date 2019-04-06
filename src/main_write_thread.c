/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: src/main_write_tread.c
 */
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <ev.h>

#include "main.h"

#define log_thread(_level, _fmt, ...) \
  log("write_thread", _level, _fmt, ##__VA_ARGS__)

#define ctx_from_loop(_ev_loop) \
  ((struct write_thread_context*)((char*)(_ev_loop) - offsetof(struct write_thread_context, loop)))

static void
sig_kill_cb(struct ev_loop *loop, ev_async *w, int revents)
{
  /*struct write_thread_context *ctx = ctx_from_loop(loop);*/
  log_thread("INFO", "catch KILL signal. Exit");
  ev_break(loop, EVBREAK_ALL);
}

void *
write_thread(struct write_thread_context *ctx)
{
  ev_run(ctx->loop, 0);
  return NULL;
}

bool
write_thread_alloc(struct write_thread_context *ctx)
{
  memset(ctx, 0, sizeof(*ctx));
  log_thread("INFO", "allocate write thread");
  ctx->loop = ev_loop_new(EVFLAG_AUTO);
  ev_async_init(&ctx->sig_kill, sig_kill_cb);
  ev_async_start(ctx->loop, &ctx->sig_kill);
  pthread_create(&ctx->thread, NULL, (void*(*)(void*))&write_thread, ctx);
  return true;
}

void
write_thread_free(struct write_thread_context *ctx)
{
  void *rval = NULL;
  /* send signal && wait */
  log_thread("INFO", "wait thread to stop...");
  ev_async_send(ctx->loop, &ctx->sig_kill);
  pthread_join(ctx->thread, &rval);

  /* free */
  ev_async_stop(ctx->loop, &ctx->sig_kill);
  ev_loop_destroy(ctx->loop);
}

