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

#define FI_INIT_VALUE {.fi_key = {'A', 'Z'}};
#define FI_KEY_VALID(_fi) ((_fi)->fi_key[0] == 'A' && (_fi)->fi_key[1] == 'Z')

struct __attribute__((packed)) timebin {
  uint64_t sec_be64;
  uint32_t usec_be32;
};

#define FH_PATH_SIZE 16
/* index record for each frame */
typedef struct __attribute__((packed)) frame_index {
  char fi_key[2];
  struct timebin tv;
  uint64_t offset_be64;
  uint32_t size_be32;
  uint64_t seq_be64;
} frame_index_t;

#define FH_INIT_VALUE {.fh_key = {'S', 'W', 'I', 'C'}}
#define FH_KEY_VALID(_fh) (!memcmp((_fh)->fh_key, "SWIC", 4))

/* file header */
typedef struct __attribute__((packed)) frame_header {
  char fh_key[4];
  /* sequence */
  uint32_t seq_be32;
  uint32_t seq_limit_be32;
  /* path to frames file */
  uint8_t path[FH_PATH_SIZE];
  struct {
    /* time of seq=0 frame in UTC time */
    struct timebin utc;
    /* first frame time in frames pack after start capture */
    struct timebin local;
  } cap_time;

  struct {
    /* frames per seconds */
    uint8_t fps;
    uint16_t width_be;
    uint16_t height_be;
  } frame;
} frame_header_t;

static inline void
timebin_from_timeval(struct timebin *tb, struct timeval *tv)
{
  tb->sec_be64 = BSWAP_BE64((uint64_t)tv->tv_sec);
  tb->usec_be32 = BSWAP_BE32((uint32_t)(tv->tv_usec));
}

static inline void
timebin_to_timeval(struct timebin *tb, struct timeval *tv)
{
  tv->tv_sec = BSWAP_BE64(tb->sec_be64);
  tv->tv_usec = BSWAP_BE32(tb->usec_be32);
}

#define TV_FMT "%"PRIu64".%.06"PRIu64
#define TV_ARGS(_tv) ((uint64_t)(_tv)->tv_sec), ((uint64_t)(_tv)->tv_usec)

#endif /* _FRAME_INDEX_1551786768_H_ */

