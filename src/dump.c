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


#include "frame_index.h"

/* print header struct
 * return false when *fh is invalid
 */
bool
dump_fh(frame_header_t *fh)
{
  float tdiff = timebin_to_float(&fh->gtime) - timebin_to_float(&fh->ltime);
  printf("# HEADER < fps = %u, time diff from global = %s%.3f >\n",
         fh->fps,
         tdiff > 0 ? "+" : "",
         tdiff);
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
  static float second = 0.f;
  static unsigned fps = 0u;

  float time = timebin_to_float(&fi->tv);
  float ptime = timebin_to_float(&pfi->tv);

  uint64_t offset = BSWAP_BE64(fi->offset_be64);
  uint32_t size = BSWAP_BE32(fi->size_be32);

  uint64_t poffset = BSWAP_BE64(pfi->offset_be64);
  uint32_t psize = BSWAP_BE32(pfi->size_be32);

  if (!FI_KEY_VALID(fi)) {
    printf("[%6llu] invalid magic key\n", seq);
    return false;
  }

  if (BSWAP_BE16(fi->tv.nsec_be16) > 1000) {
    printf("[%6llu] invalid microseconds second value\n", seq);
    return false;
  }

  if (ptime > time) {
    printf("[%6llu] frame time invalid (%.3f < %.3f)\n", seq, time, ptime);
    return false;
  }

  if (poffset + psize > offset) {
    printf("[%6llu] offset value invalid: previous frame end > offset: %"PRIu64" > %"PRIu64,
           seq, poffset + psize, offset);
    return false;
  }

  printf("[%6llu] { time = %.3f, offset = %10"PRIu64", size = %10"PRIu32" } time diff: %.3f\n",
         seq, time, offset, size, time - ptime);

  second += time - ptime;
  fps++;
  if (second >= 1.0f) {
    printf("# fps = %u, time counted = %.3f\n", fps, second);
    second = 0.0f;
    fps = 0u;
  }

  seq++;
  memcpy(pfi, fi, sizeof(*fi));
  return true;
}


int
main(int argc, char *argv[])
{
  int fd;
  int r;
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

  /* read header */
  r = read(fd, &fh, sizeof(fh));
  if (r != sizeof(fh)) {
    printf("# header: unexpected end: %d bytes readed, expected %d\n",
           r, (int)sizeof(fh));
    close(fd);
    return EXIT_FAILURE;
  }

  if (!dump_fh(&fh)) {
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

