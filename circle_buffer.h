/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: circle_buffer.h
 */
#ifndef _CIRCLE_BUFFER_1551711442_H_
#define _CIRCLE_BUFFER_1551711442_H_
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

struct circle_buffer {
  /* pointer to start of data */
  uint8_t *p;
  /* pointer to end of buffer (= p + capacity) */
  uint8_t *e;
  /* size of allocated buffer */
  size_t capacity;
  size_t free_space;

  /* pointer to start stored area */
  uint8_t *start_p;
  /* pointer end stored data */
  uint8_t *end_p;
};

bool
cbf_is_empty(struct circle_buffer *cbf);

bool
cbf_init(struct circle_buffer *cb, size_t capacity);

void
cbf_dump(struct circle_buffer *cb);

/*
 * store data to buffer
 * return stored len
 */
size_t
cbf_save(struct circle_buffer *cb, uint8_t *p, size_t len);

/*
 * get data from buffer, without free readed data (need discard to free)
 * return readed data
 */
size_t
cbf_get(struct circle_buffer *cb, uint8_t *p, size_t len);

/* discard data in buffer
 * return discarded len
 */
size_t
cbf_discard(struct circle_buffer *cb, size_t len);

#endif /* _CIRCLE_BUFFER_1551711442_H_ */

