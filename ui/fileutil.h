/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef FILEUTIL_H
#define FILEUTIL_H
#include <stddef.h>

/* Copy src -> dst atomically and durably.
 *
 * Writes to a unique temporary beside dst, fsyncs it, then renames. dst therefore
 * only ever exists as a complete file: a reader can never see a partial copy, two
 * concurrent copies to the same dst cannot interleave, and a power cut cannot leave
 * a truncated file behind under a name that looks valid.
 *
 * max_bytes = 0 means no size limit. A source larger than max_bytes is refused
 * without writing anything.
 *
 * Returns 0 on success, -1 on any failure (with the temporary removed). */
int file_copy_atomic(const char *src, const char *dst, size_t max_bytes);

#endif
