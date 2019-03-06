/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: src/dump.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/time.h>

#include "frame_index.h"

/* print header struct
 * return false when *fh is invalid
 */
bool
dump_fh(frame_header_t *fh, size_t file_size)
{
  struct timeval tv_diff = {0};
  struct timeval ltime;
  struct timeval gtime;
  struct timeval fft;
  char symbol = '+';
  timebin_to_timeval(&fh->cap_time.local, &ltime);
  timebin_to_timeval(&fh->cap_time.utc, &gtime);
  timebin_to_timeval(&fh->first_frame_time, &fft);

  if (timercmp(&ltime, &gtime, >)) {
    timersub(&ltime, &gtime, &tv_diff);
    symbol = '-';
  } else if (timercmp(&ltime, &gtime, <)) {
    timersub(&gtime, &ltime, &tv_diff);
    symbol = '+';
  }

  printf("# HEADER < "
         "frames = %zu, fps = %u, fft = "TV_FMT", "
         "local time = "TV_FMT", UTC diff = %c"TV_FMT" "
         ">\n",
         (file_size - sizeof(frame_header_t)) / sizeof(frame_index_t),
         fh->fps,
         TV_ARGS(&fft),
         TV_ARGS(&ltime),
         symbol,
         TV_ARGS(&tv_diff));
  return true;
}

/* print readed struct
 * pfi: previous struct
 * fi: current struct
 * return false when *fi is invalid
 */
bool
dump_fi(frame_index_t *pfi, frame_index_t *fi)
{
  static long long unsigned int seq = 1u;
  static unsigned fps = 0u;
  int errors = 0;

  const struct timeval one_second = {.tv_sec = 1};
  struct timeval second;
  struct timeval tv_diff = {0};
  struct timeval ctime;
  struct timeval ptime;

  timebin_to_timeval(&fi->tv, &ctime);
  timebin_to_timeval(&pfi->tv, &ptime);

  uint64_t offset = BSWAP_BE64(fi->offset_be64);
  uint32_t size = BSWAP_BE32(fi->size_be32);

  uint64_t poffset = BSWAP_BE64(pfi->offset_be64);
  uint32_t psize = BSWAP_BE32(pfi->size_be32);

  uint64_t frame_seq = BSWAP_BE64(fi->seq_be64);

  if (!FI_KEY_VALID(fi)) {
    printf("[%6llu] invalid magic key: %x\n", seq, BSWAP_BE16(*((uint16_t*)fi->key)));
    errors++;
  }

  if (BSWAP_BE16(fi->tv.usec_be32) > 1000000) {
    printf("[%6llu] invalid microseconds value: %"PRIu32"\n",
           seq, BSWAP_BE16(fi->tv.usec_be32));
    errors++;
  }

  if (!timercmp(&ptime, &ctime, <)) {
    printf("[%6llu] frame time invalid ("TV_FMT" < "TV_FMT")\n",
           seq, TV_ARGS(&ctime), TV_ARGS(&ptime));
    errors++;
  }

  if (poffset + psize > offset) {
    printf("[%6llu] offset value invalid: previous frame end > offset: %"PRIu64" > %"PRIu64"\n",
           seq, poffset + psize, offset);
    errors++;
  }

  timersub(&ctime, &ptime, &tv_diff);
  printf("[%6llu] { %6"PRIu64" time = "TV_FMT", offset = %10"PRIu64", size = %10"PRIu32" } time diff: "TV_FMT"\n",
         seq, frame_seq, TV_ARGS(&ctime), offset, size, TV_ARGS(&tv_diff));

  timeradd(&second, &tv_diff, &second);
  fps++;
  if (!timercmp(&second, &one_second, <)) {
    printf("# fps = %u, time counted = "TV_FMT"\n", fps, TV_ARGS(&second));
    timerclear(&second);
    fps = 0u;
  }

  if (errors)
    return false;

  seq++;
  memcpy(pfi, fi, sizeof(*fi));
  return true;
}


int
main(int argc, char *argv[])
{
  int fd;
  int r;
  size_t file_size = 0u;
  frame_header_t fh = FH_INIT_VALUE;
  frame_index_t fi = FI_INIT_VALUE;
  frame_index_t pfi = FI_INIT_VALUE;

  if (argc < 2) {
    printf("usage: <file name>\n");
    return EXIT_FAILURE;
  }

  fd = open(argv[1], O_RDONLY);
  if (fd == -1) {
    perror("open");
    return EXIT_FAILURE;
  }

  file_size = lseek(fd, 0u, SEEK_END);
  lseek(fd, 0u, SEEK_SET);

  /* read header */
  r = read(fd, &fh, sizeof(fh));
  if (r != sizeof(fh)) {
    printf("# header: unexpected end: %d bytes readed, expected %d\n",
           r, (int)sizeof(fh));
    close(fd);
    return EXIT_FAILURE;
  }

  if (!dump_fh(&fh, file_size)) {
    printf("# header: invalid data\n");
    return EXIT_FAILURE;
  }

  /* read frames */
  while (true) {
    r = read(fd, &fi, sizeof(fi));
    if (r == 0) {
      printf("EOF\n");
      break;
    }
    if (r != sizeof(fi)) {
      printf("# index: unexpected end: %d bytes readed, expected %d\n",
             r, (int)sizeof(fi));
      close(fd);
      return EXIT_FAILURE;
    }

    if (!dump_fi(&pfi, &fi)) {
      printf("# index: invalid data\n");
      break;
    }
  }

  close(fd);
  return EXIT_SUCCESS;
}

