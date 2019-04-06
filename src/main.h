/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: src/main.h
 */
#ifndef _SRC_MAIN_1554547715_H_
#define _SRC_MAIN_1554547715_H_

#define log(_module, _level, _fmt, ...) \
  fprintf(stderr, "%s: [%s] " _fmt "\n", _level, _module, ##__VA_ARGS__)


struct write_thread_context {
  struct ev_loop *loop;
  struct ev_async sig_kill;
  pthread_t thread;
};

extern bool write_thread_alloc(struct write_thread_context *ctx);
extern void write_thread_free(struct write_thread_context *ctx);

#endif /* _SRC_MAIN_1554547715_H_ */

