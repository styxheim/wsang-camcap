/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: src/extract.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>

#include "files.h"
#include "frame_index.h"

#define EXTRACT_BLK_SZ 4096
/* write to stdout */
#define OUTPUT_FD STDOUT_FILENO

struct walk_context {
  int fd;

  int output_fd;
  time_t start_time;
  time_t duration;
  struct timeval local_start;
  struct timeval local_end;

  uint32_t file_seq;
  uint32_t file_seq_limit;

  uint64_t frame_seq;

  char frm_path[FH_PATH_SIZE + 1];

  struct {
    int fd;
    char path[FH_PATH_SIZE + 1];
  } frm_ctx;
};

void
dump_frame_index(frame_index_t *pfi)
{
  struct timeval tv;
  timebin_to_timeval(&pfi->tv, &tv);
  fprintf(stderr, "INFO: frame [%6"PRIu64"] { "
          "time = "TV_FMT", offset = %10"PRIu64", size = %10"PRIu32" "
          "}\n",
          BSWAP_BE64(pfi->seq_be64),
          TV_ARGS(&tv),
          BSWAP_BE64(pfi->offset_be64),
          BSWAP_BE32(pfi->size_be32));
}

bool
dump_frame(struct walk_context *wlkc, frame_index_t *pfi)
{
  off_t offset;
  char buffer[EXTRACT_BLK_SZ];
  size_t readed;
  size_t frame_size;
  int expect;
  int r;

  if (strcmp(wlkc->frm_ctx.path, wlkc->frm_path)) {
    if (!wlkc->frm_ctx.path[0])
      fprintf(stderr, "INFO: open frm pack '%s'\n", wlkc->frm_path);
    else
      fprintf(stderr, "INFO: change frm pack '%s' to '%s'\n",
              wlkc->frm_ctx.path, wlkc->frm_path);
    if (wlkc->frm_ctx.fd != -1)
      close(wlkc->frm_ctx.fd);
    memcpy(wlkc->frm_ctx.path, wlkc->frm_path, FH_PATH_SIZE + 1);
    wlkc->frm_ctx.fd = open(wlkc->frm_ctx.path, O_RDONLY);
    if (wlkc->frm_ctx.fd == -1) {
      fprintf(stderr, "ERROR: open frm file '%s' failed: %s\n",
              wlkc->frm_ctx.path, strerror(errno));
      return false;
    }
  }
  
  offset = BSWAP_BE64(pfi->offset_be64);
  dump_frame_index(pfi);
  if (lseek(wlkc->frm_ctx.fd, offset, SEEK_SET) != offset) {
    fprintf(stderr, "ERROR: seek to frame start (%"PRIu64") not possible\n",
            (uint64_t)offset);
    return false;
  }

  readed = 0u;
  frame_size = BSWAP_BE32(pfi->size_be32);
  while (readed != frame_size) {
    expect = (frame_size - readed) % EXTRACT_BLK_SZ;
    if (!expect)
      expect = EXTRACT_BLK_SZ;
    r = read(wlkc->frm_ctx.fd, buffer, expect);
    if (r == -1) {
      fprintf(stderr, "ERROR: frm read failure: %s\n", strerror(errno));
      return false;
    }
    readed += r;
    if (r < expect) {
      fprintf(stderr, "ERROR: frm unexpected EOF: readed=%zu, expected=%zu (%d, %d)\n",
             readed, frame_size, r, expect);
    }

    if (wlkc->output_fd != -1) {
      expect = r;
      r = write(wlkc->output_fd, buffer, expect);
      if (r == -1 || r == 0) {
        fprintf(stderr, "ERROR: write failure %s\n", strerror(errno));
        return false;
      }
    }
  }

  return true;
}

bool
frame_index_seek_down(struct walk_context *wlkc, frame_index_t *pfi)
{
  off_t r;
  struct timeval tv;

  /* seek until frame header */
  do {
    r = lseek(wlkc->fd, -(sizeof(*pfi) * 2), SEEK_CUR);
    read(wlkc->fd, pfi, sizeof(*pfi));
    /* dump_frame_index(pfi); */
    timebin_to_timeval(&pfi->tv, &tv);
    if (timercmp(&wlkc->local_start, &tv, <)) {
      /* search next */
      continue;
    } else if (!timercmp(&wlkc->local_start, &tv, !=)) {
      /* founded equal */
      return true;
    } else {
      /* start at next frame */
      read(wlkc->fd, pfi, sizeof(*pfi));
      return true;
    }
  } while (r >= sizeof(frame_header_t));
  return false;
}

bool
frame_index_seek_up(struct walk_context *wlkc, frame_index_t *pfi)
{
  struct timeval tv;
  int r = 0;

  do {
    r = read(wlkc->fd, pfi, sizeof(*pfi));
    /* dump_frame_index(pfi); */
    timebin_to_timeval(&pfi->tv, &tv);
    if (timercmp(&wlkc->local_start, &tv, >)) {
      /* search next */
      continue;
    } else {
      /* found previous or equal */
      return true;
    }
  } while (r != 0);
  return false;
}

bool
frame_index_open_next(struct walk_context *wlkc)
{
  uint32_t next_idx = wlkc->file_seq + 1;
  int new_fd;
  char path[FH_PATH_SIZE + 1];
  frame_header_t fh;
  if (wlkc->file_seq_limit)
    next_idx %= wlkc->file_seq_limit;

  make_idx_file(path, next_idx);
  fprintf(stderr, "INFO: open next file: %s\n", path);
  new_fd = open(path, O_RDONLY);
  if (new_fd == -1) {
    fprintf(stderr, "ERROR: open '%s' failed: %s\n", path, strerror(errno));
    return false;
  }
  close(wlkc->fd);
  wlkc->fd = new_fd;
  wlkc->file_seq = next_idx;

  if (read(new_fd, &fh, sizeof(fh)) != sizeof(fh)) {
    fprintf(stderr, "ERROR: incomplete data: stripped frame header\n");
    return false;
  }
  
  if (BSWAP_BE32(fh.seq_be32) != wlkc->file_seq) {
    fprintf(stderr, "ERROR: inconsistent sequence: "
            "received != expected: %"PRIu32" != %"PRIu32"\n",
            BSWAP_BE32(fh.seq_be32), wlkc->file_seq);
    return false;
  }

  if (BSWAP_BE32(fh.seq_limit_be32) != wlkc->file_seq_limit) {
    fprintf(stderr, "ERROR: inconsistent sequence limit: "
            "received != expected: %"PRIu32" != %"PRIu32"\n",
            BSWAP_BE32(fh.seq_limit_be32), wlkc->file_seq);
    return false;
  }

  snprintf(wlkc->frm_path, sizeof(wlkc->frm_path) - 1, "%s", fh.path);

  return true;
}

/* dump frames until end */
void
frame_index_walk_until_end(struct walk_context *wlkc, frame_index_t *pfi)
{
  struct timeval tv = {0};

  dump_frame(wlkc, pfi);
  wlkc->frame_seq = BSWAP_BE64(pfi->seq_be64);
  while (timercmp(&wlkc->local_end, &tv, >))
  {
    if (read(wlkc->fd, pfi, sizeof(*pfi)) == 0) {
      /* TODO: open next file in sequence */
      if (!frame_index_open_next(wlkc)) {
        fprintf(stderr, "ERROR: anormal result when switching to next frame pack\n");
        return;
      }
      continue;
    }
    if (BSWAP_BE64(pfi->seq_be64) != wlkc->frame_seq + 1) {
      fprintf(stderr, "ERROR: invalid frame sequence: "
              "expected: %"PRIu64" received: %"PRIu64"\n",
              BSWAP_BE64(pfi->seq_be64), wlkc->frame_seq + 1);
      return;
    }
    timebin_to_timeval(&pfi->tv, &tv);
    wlkc->frame_seq++;
    dump_frame(wlkc, pfi);
  }
}

void
frame_index_walk(struct walk_context *wlkc)
{
  struct timeval frame_time;
  frame_index_t fi = {0};
  read(wlkc->fd, &fi, sizeof(frame_index_t));
  
  if (!FI_KEY_VALID(&fi)) {
    fprintf(stderr, "WARN: invalid frame index magic key\n");
    return;
  }

  timebin_to_timeval(&fi.tv, &frame_time);
  if (timercmp(&wlkc->local_start, &frame_time, <)) {
    /* TODO: seek down */
    if (!frame_index_seek_down(wlkc, &fi)) {
      fprintf(stderr, "ERROR: start frame not found\n");
      return;
    }
  } else if (timercmp(&wlkc->local_start, &frame_time, >)) {
    /* TODO: seek up */
    if (!frame_index_seek_up(wlkc, &fi)) {
      fprintf(stderr, "ERROR: start frame not found\n");
      return;
    }
  }

  frame_index_walk_until_end(wlkc, &fi);
}

bool
index_process(struct walk_context *wlkc,
              const char *filepath, size_t frame_count,
              frame_header_t *fh, frame_index_t *fi)
{
  struct timeval fh_local;
  struct timeval fh_utc;
  struct timeval frame_time;
  /* make start time */
  timerclear(&wlkc->local_start);
  wlkc->local_start.tv_sec = wlkc->start_time;
  timebin_to_timeval(&fh->cap_time.utc, &fh_utc);
  timebin_to_timeval(&fh->cap_time.local, &fh_local);
  timeradd(&fh_utc, &fh_local, &frame_time);

  /* check start time */
  if (timercmp(&frame_time, &wlkc->local_start, >)) {
    fprintf(stderr, "INFO: skip file '%s', record start time > request start time "
           "("TV_FMT" > "TV_FMT")\n",
           filepath, TV_ARGS(&frame_time), TV_ARGS(&wlkc->local_start));
    return true;
  }

  /* convert global time to local */
  timersub(&wlkc->local_start, &fh_utc, &wlkc->local_start);
  timerclear(&wlkc->local_end);
  wlkc->local_end.tv_sec = wlkc->local_start.tv_sec + wlkc->duration;
  wlkc->local_end.tv_usec = wlkc->local_start.tv_usec;
  
  timebin_to_timeval(&fi->tv, &frame_time);
  /* check end time */
  if (timercmp(&frame_time, &wlkc->local_start, <)) {
    fprintf(stderr, "INFO: skip file '%s', last frame time < relative request start time "
           "("TV_FMT" < "TV_FMT")\n",
           filepath, TV_ARGS(&frame_time), TV_ARGS(&wlkc->local_start));
    return true;
  }

  /* FIXME: check fi[0]->tv == fh->cap_time.local ? */

  fprintf(stderr, "INFO: use file '%s' relative { start = "TV_FMT", end = "TV_FMT" }\n",
         filepath, TV_ARGS(&wlkc->local_start), TV_ARGS(&wlkc->local_end));

  wlkc->file_seq = BSWAP_BE32(fh->seq_be32);
  wlkc->file_seq_limit = BSWAP_BE32(fh->seq_limit_be32);
  snprintf(wlkc->frm_path, sizeof(wlkc->frm_ctx) - 1, "%s", fh->path);

  /* seek to frame */
  {
    off_t _seek_to_frame;
    _seek_to_frame = (fh->frame.fps * (wlkc->local_start.tv_sec - fh_local.tv_sec));
    if (_seek_to_frame > frame_count) {
      _seek_to_frame = frame_count - 1;
    }
    _seek_to_frame = sizeof(frame_header_t) +
                     (sizeof(frame_index_t) * _seek_to_frame);
    lseek(wlkc->fd, _seek_to_frame, SEEK_SET);
  }
  frame_index_walk(wlkc);

  /* correct start time */
  timersub(&wlkc->local_start, &fh_utc, &wlkc->local_start);
  return false;
}

/* return `true` when walk not stopped */
bool
index_walk(struct walk_context *wlkc, const char *filepath)
{
  bool r = false;
  size_t frame_count = 0u;
  frame_header_t fh = {0};
  frame_index_t fi = {0};

  wlkc->fd = open(filepath, O_RDONLY);
  if (wlkc->fd == -1) {
    fprintf(stderr, "WARN: file '%s' not oppened: %s\n", filepath, strerror(errno));
    /* try next */
    return true;
  }

  /* get last record */
  frame_count = lseek(wlkc->fd, -sizeof(frame_index_t), SEEK_END);
  read(wlkc->fd, &fi, sizeof(fi));
  if (!FI_KEY_VALID(&fi)) {
    fprintf(stderr, "WARN: file '%s' has invalid last record magic key\n", filepath);
    close(wlkc->fd);
    /* try next */
    return true;
  }
  lseek(wlkc->fd, 0, SEEK_SET);
  
  /* compute frames */
  frame_count = (frame_count - sizeof(frame_header_t)) / sizeof(frame_index_t);

  /* get header */
  read(wlkc->fd, &fh, sizeof(fh));
  if (!FH_KEY_VALID(&fh)) {
    fprintf(stderr, "WARN: file '%s' has invalid header magic key\n", filepath);
    close(wlkc->fd);
    /* try next */
    return true;
  }

  r = index_process(wlkc, filepath, frame_count, &fh, &fi);
  close(wlkc->fd);
  if (wlkc->frm_ctx.fd != -1)
    close(wlkc->frm_ctx.fd);
  return r;
}

bool
dir_walk(struct walk_context *wlkc, const char *dirpath)
{
  DIR *dirp;
  struct dirent *rd;
  size_t file_count = 0u;

  if ((dirp = opendir(dirpath)) == NULL) {
    fprintf(stderr, "ERROR: directory not openned: %s\n", strerror(errno));
    return false;
  }

  while ((rd = readdir(dirp)) != NULL) {
    if (!strncmp(rd->d_name, FILE_IDX_PREFIX, sizeof(FILE_IDX_PREFIX) - 1)) {
      file_count++;
      if (!index_walk(wlkc, rd->d_name))
        return false;
    }
  }

  closedir(dirp);

  if (!file_count) {
    fprintf(stderr, "INFO: no frame index files found in path: '%s'\n", dirpath);
  }
  return true;
}

int
main(int argc, char *argv[])
{

  struct walk_context wlkc = {0};
  wlkc.fd = -1;
  wlkc.frm_ctx.fd = -1;
  wlkc.output_fd = OUTPUT_FD;

  if (argc < 3) {
    fprintf(stderr, "Extract frames to stdout from current directory\n");
    fprintf(stderr, "usage: <utc_seconds_start> <seconds_duration>\n");
    return EXIT_FAILURE;
  }

  if (isatty(wlkc.output_fd)) {
    wlkc.output_fd = -1;
    fprintf(stderr, "INFO: disabling dump frames. Output is terminal\n");
  }

  wlkc.start_time = (time_t)strtoul(argv[1], NULL, 10);
  wlkc.duration = (time_t)strtoul(argv[2], NULL, 10);

  {
    time_t end_time;
    struct tm *tm_start;
    struct tm *tm_end;
    char bf_start[32];
    char bf_end[32];

    end_time = wlkc.start_time + wlkc.duration;
    tm_start = gmtime(&wlkc.start_time);
    tm_end = gmtime(&end_time);
    strftime(bf_start, sizeof(bf_start), "%H:%M:%S", tm_start);
    strftime(bf_end, sizeof(bf_start), "%H:%M:%S", tm_end);

    fprintf(stderr, "INFO: get frames from %s to %s (%"PRIu64" seconds)\n",
            bf_start, bf_end, (uint64_t)wlkc.duration);
  }

  dir_walk(&wlkc, ".");

	return EXIT_SUCCESS;
}

