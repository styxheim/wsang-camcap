/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: circle_buffer.c
 */
#include <string.h>
#include <stdlib.h>

#include "circle_buffer.h"

bool
cbf_is_empty(struct circle_buffer *cbf)
{
  return cbf->free_space == cbf->capacity;
}

bool
cbf_init(struct circle_buffer *cbf, size_t capacity)
{
  memset(cbf, 0u, sizeof(*cbf));
  cbf->p = calloc(capacity, sizeof(uint8_t));
  if (!cbf->p)
    return false;
  cbf->capacity = capacity;
  cbf->free_space = capacity;
  cbf->end_p = cbf->p;
  cbf->e = cbf->p + capacity;
  cbf->start_p = cbf->e;
  return true;
}

void
cbf_destroy(struct circle_buffer *cbf)
{
  free(cbf->p);
  memset(cbf, 0, sizeof(*cbf));
}

void
cbf_dump(struct circle_buffer *cbf)
{
  unsigned i = 0;
  const unsigned line_limit = 8u;

  printf("buffer %p {\n", (void*)cbf);
  printf("  capacity = %zu\n", cbf->capacity);
  printf("  free_space = %zu\n", cbf->free_space);
  printf("  p = %p, e = %p\n", (void*)cbf->p, (void*)cbf->e);
  printf("  start_p = %p, end_p = %p\n", (void*)cbf->p, (void*)cbf->e);
  printf("}\n");
  
  printf("Raw data:");

  for (i = 0; i < cbf->capacity; i++) {
    char c1 = ' ';
    char c2 = ' ';
    if (!(i % line_limit)) {
      printf("    eol_p = %p ", &cbf->p[i]);
      printf("\n%08x  ", i);
    }
    if (&cbf->p[i] == cbf->start_p)
      c2 = '[';
    if (&cbf->p[i] == cbf->end_p)
      c1 = ']';
    printf("%c%c%02x", c1, c2, cbf->p[i]);
  }
  
  if (i % line_limit) {
    unsigned tail_i = i;
    for (; tail_i % line_limit; tail_i++) {
      char c1 = ' ';
      char c2 = ' ';
      if (&cbf->p[tail_i] == cbf->start_p)
        c2 = '[';
      if (&cbf->p[tail_i] == cbf->end_p)
        c1 = ']';

      printf("%c%cxx", c1, c2);
    }
    printf("    eol_p = %p ", &cbf->p[i]);
  }

  printf("\n");

}

size_t
cbf_discard(struct circle_buffer *cbf, size_t len)
{
  size_t zeros = 0;
  if (cbf->free_space == cbf->capacity) {
    /* buffer is empty */
    return 0;
  }

  for (; zeros < len && cbf->free_space < cbf->capacity; zeros++) {
    if (cbf->start_p >= cbf->e)
      cbf->start_p = cbf->p;

    *(cbf->start_p++) = 0;
    cbf->free_space++;
  }
  return zeros;
}

size_t
cbf_get(struct circle_buffer *cbf, uint8_t *p, size_t len)
{
  if (cbf->free_space == cbf->capacity) {
    /* buffer is empty */
    return 0;
  }

  if (len > cbf->capacity - cbf->free_space) {
    /* limit len by stored data size */
    len = cbf->capacity - cbf->free_space;
  }

  if (cbf->start_p >= cbf->end_p && cbf->start_p + len >= cbf->e) {
    /* copy over end */
    size_t first_piece_len = cbf->e - cbf->start_p;
    size_t second_piece_len = len - first_piece_len;
    if (first_piece_len)
      memcpy(p, cbf->start_p, first_piece_len);
    memcpy(p + first_piece_len, cbf->p, second_piece_len);
  } else {
    /* simple copy */
    memcpy(p, cbf->start_p, len);
  }

  return len;
}

size_t
cbf_save(struct circle_buffer *cbf, uint8_t *p, size_t len)
{
  if (cbf->free_space < len) {
    /* simple behavior: discard data */
    return 0u;
  }

  if (cbf->end_p >= cbf->start_p && cbf->end_p + len >= cbf->e) {
    /* copy in two step: before end of buffer and after of start */
    size_t first_piece_len = cbf->e - cbf->end_p;
    size_t second_piece_len = len - first_piece_len;
    if (first_piece_len)
      memcpy(cbf->end_p, p, first_piece_len);
    memcpy(cbf->p, p + first_piece_len, second_piece_len);
    cbf->end_p = cbf->p + second_piece_len;
  } else {
    /* simple copy */
    memcpy(cbf->end_p, p, len);
    cbf->end_p += len;
  }
  cbf->free_space -= len;
  return len;
}

#if SELF_TEST

void
build_buffer(uint8_t c, uint8_t *buf, size_t len)
{
  size_t i = 0;
  for (; i < len; i++) {
    if (!((uint8_t)i)) {
      buf[i] = (uint8_t)c;
      c++;
    } else
      buf[i] = (uint8_t)i;
  }
}

#include <assert.h>

int main() {
  struct circle_buffer cbf;
 
  size_t len = 0u;
  uint8_t buf1[50];
  uint8_t buf2[50];
  uint8_t buf3[49];
  uint8_t buf4[65];
  uint8_t buf5[100];

  build_buffer(1, buf1, sizeof(buf1));
  build_buffer(2, buf2, sizeof(buf2));
  build_buffer(3, buf3, sizeof(buf3));
  build_buffer(4, buf4, sizeof(buf4));
  build_buffer(5, buf5, sizeof(buf5));

  cbf_init(&cbf, 100);
  cbf_dump(&cbf);

  len = cbf_save(&cbf, buf1, sizeof(buf1));
  cbf_dump(&cbf);
  assert(len == sizeof(buf1));
  
  len = cbf_save(&cbf, buf2, sizeof(buf2));
  cbf_dump(&cbf);
  assert(len == sizeof(buf2));

  len = cbf_discard(&cbf, 30);
  cbf_dump(&cbf);
  assert(len == 30);

  len = cbf_discard(&cbf, 20);
  cbf_dump(&cbf);
  assert(len == 20);

  len = cbf_save(&cbf, buf3, sizeof(buf3));
  cbf_dump(&cbf);
  assert(len == sizeof(buf3));

  len = cbf_save(&cbf, buf1, 2);
  cbf_dump(&cbf);
  assert(len == 0);

  len = cbf_save(&cbf, buf1, 1);
  cbf_dump(&cbf);
  assert(len == 1);

  len = cbf_discard(&cbf, 200);
  cbf_dump(&cbf);
  assert(len == 100);

  len = cbf_save(&cbf, buf4, sizeof(buf4));
  cbf_dump(&cbf);
  assert(len == sizeof(buf4));

  {
    uint8_t i = 1u;
    while (cbf_save(&cbf, &i, 1u) != 0u) i++;
    cbf_dump(&cbf);
  }

  cbf_discard(&cbf, 100);
  cbf_dump(&cbf);

  len = cbf_save(&cbf, buf5, sizeof(buf5));
  cbf_dump(&cbf);
  assert(len == sizeof(buf5));

  {
    uint8_t buf5_c[sizeof(buf5)] = {0};
    cbf_get(&cbf, buf5_c, sizeof(buf5_c));
    assert(!memcmp(buf5_c, buf5, sizeof(buf5)));
  }

  cbf_destroy(&cbf);
  return 0;
}

#endif

