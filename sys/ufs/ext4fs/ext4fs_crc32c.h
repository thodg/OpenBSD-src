/* ext4fs_crc32c.h
 * CRC32C (Castagnoli) implementation for ext4fs
 *
 * Copyright 2025 kmx.io <contact@kmx.io>
 *
 * Permission is hereby granted to use this software granted the above
 * copyright notice and this permission paragraph are included in all
 * copies and substantial portions of this software.
 *
 * THIS SOFTWARE IS PROVIDED "AS-IS" WITHOUT ANY GUARANTEE OF
 * PURPOSE AND PERFORMANCE. IN NO EVENT WHATSOEVER SHALL THE
 * AUTHOR BE CONSIDERED LIABLE FOR THE USE AND PERFORMANCE OF
 * THIS SOFTWARE.
 */
#ifndef _EXT4FS_CRC32C_H_
#define _EXT4FS_CRC32C_H_

#include <sys/types.h>

/*
 * CRC32C uses the Castagnoli polynomial: 0x1EDC6F41
 * This is different from the standard CRC32 (ISO 3309) polynomial.
 *
 * ext4 stores checksums as the bitwise inverse of the CRC32C value.
 */

/* Compute CRC32C of a buffer, starting from an initial CRC value */
u_int32_t ext4fs_crc32c(u_int32_t crc, const void *buf, size_t len);

/* Compute CRC32C with initial value of ~0, then invert result (ext4 style) */
u_int32_t ext4fs_crc32c_le(u_int32_t crc, const void *buf, size_t len);

#endif /* _EXT4FS_CRC32C_H_ */
