/* vim: ft=c ff=unix fenc=utf-8 ts=2 sw=2 et
 * file: files.h
 */
#ifndef _FILES_1551981506_H_
#define _FILES_1551981506_H_

#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>

#include "frame_index.h"

#define FILE_IDX_PREFIX "idx_"
#define FILE_FRM_PREFIX "frm_"

/* make filename for frame index database */
static inline void
make_idx_file(char buf[FH_PATH_SIZE + 1], uint32_t sequence)
{
  snprintf(buf, FH_PATH_SIZE + 1, FILE_IDX_PREFIX "%010"PRIu32, sequence);
}

static inline void
make_frm_file(char buf[FH_PATH_SIZE + 1], uint32_t sequence)
{
  snprintf(buf, FH_PATH_SIZE + 1, FILE_FRM_PREFIX "%010"PRIu32, sequence);
}

#endif /* _FILES_1551981506_H_ */

