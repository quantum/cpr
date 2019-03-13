/**
 * Copyright (c) 2018-2019. Quantum Corporation. All Rights Reserved.
 * DXi, StorNext and Quantum are either a trademarks or registered
 * trademarks of Quantum Corporation in the US and/or other countries.
 *
 * @brief FICLONE/FICLONERANGE test library.
 *
 * @note This file is standard C11. It does not use any Quantum-specific
 *       code or libraries so it can be called on BTRFS/ext4/XFS file-systems
 *       on non-DXi platforms. Compile with @e -D_GNU_SOURCE=1.
 *
 * @section license License
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * @section notes Notes
 *
 * Pretty-much all of the errors that could be returned from
 * FICLONE/FICLONERANGE indicate that reflink @e may not be supported.
 *
 * - EXDEV (src and dst on different FS) -> need to deep copy.
 * - ENOSYS (FICLONE ioctl missing) -> need to deep copy bytes.
 * - EBADF (src unreadable, dst unwritable or dst on non-reflink FS) ->
 *   may need to deep copy.
 * - EINVAL (fs doesn't support reflink, or perhaps the block alignment
 *   of the requested reflink is not supported) -> need to deep copy.
 * - EPERM (dst is immutable). Also returned by write() if we deep copy.
 * - EISDIR (src or dst is a directory and FS doesn't support directory
 *   reflinks) -> try deep copy.
 * - etc.
 *
 * For real failures (e.g. dst unwritable, dst immutable) a read/write
 * deep copy will return a real error that we can tell the caller.
 */

#include "libcpr.h"

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

/*============================================================================*/

/**
 * Find the smaller of two objects that are the same type.
 *
 * @note This has the potential to double-evaluate its input parameters, so do
 *       not invoke it with parameters that have side-effects.
 */
#define MIN(x_, y_) (((x_) <= (y_)) ? (x_) : (y_))

/*============================================================================*/

/**
 * Clone a range from @p src_fd into @p dst_fd.
 *
 * @note This operation may fail for any number of reasons. Pretty-much all of
 *       them are able to indicate that the FICLONERANGE IOCTL is not
 *       available or not supported on the particular combination of
 *       parameters.
 *
 * @param[in] src_fd     Source file.
 * @param[in] dst_fd     Destination file.
 * @param[in] src_offset Offset to start clone from.
 * @param[in] dst_offset Offset to start clone to.
 * @param[in] length     Length of clone.
 * @return 0 for success, non-zero errno value on failure.
 */

static int clone_file_range_impl (const int    src_fd,
                                  const int    dst_fd,
                                  const off_t  src_offset,
                                  const off_t  dst_offset,
                                  const size_t length)

{
  struct file_clone_range clone_range =
  {
    .src_fd      = src_fd,
    .src_offset  = src_offset,
    .src_length  = length,
    .dest_offset = dst_offset
  };

  int rc = ioctl(dst_fd, FICLONERANGE, &clone_range);

  if (rc < 0)
  {
    rc = errno;
  }

  return rc;
}

/*============================================================================*/

/**
 * Clone an entire file from @p src_fd into @p dst_fd.
 *
 * @note This operation may fail for any number of reasons. Pretty-much all of
 *       them are able to indicate that the FICLONE IOCTL is not
 *       available or not supported on the particular combination of
 *       parameters.
 *
 * @param[in] src_fd Source file.
 * @param[in] dst_fd Destination file.
 * @return 0 for success, non-zero errno value on failure.
 */

static int clone_file_impl (const int src_fd, const int dst_fd)
{
  int rc = ioctl(dst_fd, FICLONE, src_fd);

  if (rc < 0)
  {
    rc = errno;
  }

  return rc;
}

/*============================================================================*/

/**
 * Seek a file descriptor, @p fd, to a desired @p offset.
 *
 * @param[in] fd     Source file.
 * @param[in] offset Offset from start.
 * @return Zero on success, some errno value on failure.
 */

static int seek_file (const int fd, const off_t offset)
{
  int rc = lseek(fd, offset, SEEK_SET);

  if (rc == -1)
  {
    rc = errno;
  }
  else if (rc > 0)
  {
    /* lseek() returns the current offset on success. We want to return zero
     * on success.
     */
    rc = 0;
  }

  return rc;
}

/*============================================================================*/

/**
 * Write a block to @p fd, blocking until the whole amount is written or
 * an error prevents writing more.
 *
 * @param[in] fd      Destination file.
 * @param[in] p_block Data to write.
 * @param[in] length  Length of data in @p p_block to write.
 * @return Zero on success, some errno value on failure.
 */

static int write_block (const int      fd,
                        const uint8_t *p_block,
                        size_t         length)
{
  int rc = 0;

  while (length > 0)
  {
    ssize_t wrote_now = write(fd, p_block, length);

    if (wrote_now < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      rc = errno;
      break;
    }

    p_block += wrote_now;
    length  -= wrote_now;
  }

  return rc;
}

/*============================================================================*/

/**
 * Copy @p length bytes from @p src_fd into @p dst_fd.
 *
 * @param[in] src_fd     Source file.
 * @param[in] dst_fd     Destination file.
 * @param[in] src_offset Offset to start copy from.
 * @param[in] dst_offset Offset to start copy to.
 * @param[in] length     Length of segment to copy. Zero to copy to source EOF.
 * @param[in] block_size Block size to use when copying.
 * @return Zero on success, some error value on failure.
 */

static int deep_copy_file_range_impl (const int    src_fd,
                                      const int    dst_fd,
                                      const off_t  src_offset,
                                      const off_t  dst_offset,
                                      const size_t length,
                                      const size_t block_size)
{
  int rc = seek_file(src_fd, src_offset);

  if (rc == 0)
  {
    rc = seek_file(dst_fd, dst_offset);
  }

  if (rc != 0)
  {
    return rc;
  }

  void *p_block = malloc(block_size * sizeof(uint8_t));

  if (p_block == NULL)
  {
    return ENOMEM;
  }

  size_t remain = (length != 0) ? length : block_size;

  while (remain > 0)
  {
    const size_t  read_max = MIN(block_size, remain);
    const ssize_t read_now = read(src_fd, p_block, read_max);

    if (read_now < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      rc = errno;
      break;
    }
    else if (read_now == 0 && length == 0)
    {
      /* EOF was reached and we were copying to EOF. Terminate loop. */
      break;
    }
    else if (read_now == 0 && remain != 0)
    {
      /* EOF was reached before the requested copy length. */
      rc = ERANGE;
      break;
    }

    rc = write_block(dst_fd, p_block, read_now);

    if (rc != 0)
    {
      break;
    }

    /* Only update the remaining length if not copying to EOF. */
    if (length != 0)
    {
      remain -= read_now;
    }
  }

  /* Cleanup. */
  free(p_block);

  return rc;
}

/*============================================================================*/

int qtm_clone_file (const int    src_fd,
                    const int    dst_fd,
                    const bool   fallback_copy,
                    const size_t fallback_copy_block_size)
{
  if (src_fd < 0 || dst_fd < 0 ||
      (fallback_copy && fallback_copy_block_size == 0))
  {
    return EINVAL;
  }

  int rc = clone_file_impl(src_fd, dst_fd);

  if (rc != 0 && fallback_copy)
  {
    rc = deep_copy_file_range_impl(src_fd, dst_fd, 0, 0, 0,
                                   fallback_copy_block_size);
  }

  return rc;
}

/*============================================================================*/

int qtm_clone_file_range (const int    src_fd,
                          const int    dst_fd,
                          const off_t  src_offset,
                          const off_t  dst_offset,
                          const size_t length,
                          const bool   fallback_copy,
                          const size_t fallback_copy_block_size)
{
  if (src_fd < 0 || dst_fd < 0 || src_offset < 0 || dst_offset < 0 ||
      (fallback_copy && fallback_copy_block_size == 0))
  {
    return EINVAL;
  }

  int rc =
    clone_file_range_impl(src_fd, dst_fd, src_offset, dst_offset, length);

  if (rc != 0 && fallback_copy)
  {
    rc = deep_copy_file_range_impl(src_fd, dst_fd, src_offset, dst_offset,
                                   length, fallback_copy_block_size);
  }

  return rc;
}

/*============================================================================*/

