/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: frame_index.h
 */
#ifndef _FRAME_INDEX_1551786768_H_
#define _FRAME_INDEX_1551786768_H_

#include <byteswap.h> 
#define FI_INIT_VALUE {.key = {'A', 'Z'}};

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

typedef struct __attribute__((packed)) frame_index {
  char key[2];
  struct {
    uint64_t sec_be64;
    uint16_t nsec_be16;
  } tv;
  uint64_t offset_be64;
  uint32_t size_be32;
} frame_index_t;


#endif /* _FRAME_INDEX_1551786768_H_ */

