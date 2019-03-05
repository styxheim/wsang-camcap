/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: frame_index.h
 */
#ifndef _FRAME_INDEX_1551786768_H_
#define _FRAME_INDEX_1551786768_H_

#include <byteswap.h> 

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
# define BSWAP_BE16(_v) bswap_16(_v)
# define BSWAP_BE32(_v) bswap_32(_v)
# define BSWAP_BE64(_v) bswap_64(_v)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
# define BSWAP_BE16(_v) (_v)
# define BSWAP_BE32(_v) (_v)
# define BSWAP_BE64(_v) (_v)
#else
# error "what?"
#endif

#define FI_INIT_VALUE {.key = {'A', 'Z'}};
#define FI_KEY_VALID(_fi) ((_fi)->key[0] == 'A' && (_fi)->key[1] == 'Z')

struct __attribute__((packed)) timebin {
  uint64_t sec_be64;
  uint16_t nsec_be16;
};

/* index record for each frame */
typedef struct __attribute__((packed)) frame_index {
  char key[2];
  struct timebin tv;
  uint64_t offset_be64;
  uint32_t size_be32;
} frame_index_t;

#define FH_INIT_VALUE {.key = {'S', 'W', 'I', 'C'}}
#define FH_KEY_VALID(_fh) (!memcmp((_fh)->key, "SWIC", 4))

/* file header */
typedef struct __attribute__((packed)) frame_header {
  char key[4];
  uint8_t fps;
  /* frame start time, result from clock_gettime() */
  struct timebin ltime;
  /* frame start time, result from gettimeofday() */
  struct timebin gtime;
} frame_header_t;

static inline void
timeval_to_timebin(struct timebin *tb, struct timeval *tv)
{
  tb->sec_be64 = BSWAP_BE64((uint64_t)tv->tv_sec);
  tb->nsec_be16 = BSWAP_BE16((uint16_t)(tv->tv_usec / 1000));
}

static inline float
timebin_to_float(struct timebin *tb)
{
  return (float)BSWAP_BE64(tb->sec_be64) +
         (float)BSWAP_BE16(tb->nsec_be16) / 1000;

}

#endif /* _FRAME_INDEX_1551786768_H_ */

