/**
 * Copyright (c) 2018-2019. Quantum Corporation. All Rights Reserved.
 * DXi, StorNext and Quantum are either a trademarks or registered
 * trademarks of Quantum Corporation in the US and/or other countries.
 *
 * @brief FICLONE/FICLONERANGE test program.
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
 * @section description Description
 *
 * This program attempts to clone a whole file or part of a file into a
 * destination file. It deliberately triggers the FICLONE IOCTL to clone a
 * whole file rather than just deferring to FICLONERANGE(0, 0, 0) to test both
 * kernel code paths.
 *
 * If the kernel does not provide either of these IOCTLs then it's possible
 * for the user to request that an old-fashioned read()/write() deep-copy be
 * performed instead.
 */

#include "libcpr.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*============================================================================*/

/**
 * Bitfield defining the various attributes to copy from the source to
 * destination file.
 *
 * @{
 */

typedef uint8_t preserve_mode_t;

#define PRESERVE_MODE_OWNER   0x01
#define PRESERVE_MODE_TIMES   0x02
#define PRESERVE_MODE_PERMS   0x04

#define PRESERVE_MODE_NONE    0x00
#define PRESERVE_MODE_ALL     (PRESERVE_MODE_OWNER | PRESERVE_MODE_TIMES | \
                               PRESERVE_MODE_PERMS)
#define PRESERVE_MODE_DEFAULT PRESERVE_MODE_NONE

/** @} */

/*============================================================================*/

/**
 * Describe whether to clone the entire file with FICLONE or just a range of
 * it with FICLONERANGE.
 */

typedef enum _clone_mode_t
{
  CLONE_MODE_FILE,
  CLONE_MODE_RANGE,
} clone_mode_t;

/*============================================================================*/

/** Structure to contain details of the entire clone operation. */

typedef struct _operation_t
{
  /**
   * Command-line supplied arguments:
   * @{
   */
  bool            fallback_copy;
  size_t          block_size;
  const char     *src_filename;
  const char     *dst_filename;
  bool            force;
  preserve_mode_t preserve_mode;
  clone_mode_t    clone_mode;
  uint64_t        src_offset;
  uint64_t        src_length;
  uint64_t        dst_offset;
  /** @} */

  /**
   * Internally generated status.
   * @{
   */
  int             src_fd;
  int             dst_fd;
  /** @} */
} operation_t;

/*============================================================================*/

/**
 * Macro to abort on some programming errors. Re-implemented here to keep
 * this program completely self-contained.
 *
 * @param[in] cond_ Assert condition. Fail if false.
 * @param[in] fmt_  Printf-style constant literal format string.
 * @param[in] ...   Arguments to match @p fmt_.
 */

#define AS(cond_, fmt_, ...)                                     \
  do {                                                           \
    if (!(cond_))                                                \
    {                                                            \
      fprintf(stderr,                                            \
              "An assertion failed at %s:%d (%s). Details:\n\n"  \
              fmt_                                               \
              "\n",                                              \
              __FILE__, __LINE__, __func__, ##__VA_ARGS__);      \
      fflush(stderr);                                            \
      abort();                                                   \
    }                                                            \
  } while (0)

/*============================================================================*/

/**
 * Display some error message followed by the usage of the program @p argv0
 * and then exit with a non-zero failure code. DOES NOT RETURN.
 *
 * @param[in] argv0 Taken from argv[0] in main().
 * @param[in] fmt   Printf-style format string for an error message. May be NULL
 *                  or an empty string if no error message is to be displayed.
 * @param[in] ...   Arguments to match @p fmt.
 */

__attribute__((noreturn))
__attribute__((format(printf, 2, 3)))

static void print_usage_and_exit (const char *argv0, const char *fmt, ...)
{
  if (fmt != NULL && fmt[0] != '\0')
  {
    fprintf(stderr, "ERROR: ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n\n");
  }

  fprintf(stderr,
          "USAGE: %s [-?] [-aotp] [-f] [-c] <SRC_FILE> <DST_FILE>             (1)\n"
          "       %s [-s SRC_OFFSET] [-d DST_OFFSET] [-l LENGTH] [-aotp] [-c] (2)\n"
          "          <SRC_FILE> <DST_FILE>\n"
          "\n"
          "WHERE:\n"
          "  SRC_FILE    Input filename.\n"
          "  DST_FILE    Output filename.\n"
          "  -a          Equivalent to -otp.\n"
          "  -c          Fall back to copy read/write copy if FICLONE fails.\n"
          "  -d          Offset into destination file to begin stitching.\n"
          "              Defaults to zero (beginning) if omitted.\n"
          "  -l          Length to copy. Defaults to zero (copy to end of\n"
          "              SRC_FILE) if omitted.\n"
          "  -o          Preserve ownership.\n"
          "  -t          Preserve timestamps.\n"
          "  -p          Preserve permissions.\n"
          "  -f          Force overwriting DST_FILE. Implied if -s,-d,-l\n"
          "              are supplied.\n"
          "  -s          Offset into source file to begin copying from.\n"
          "              Defaults to zero (beginning) if omitted.\n"
          "  -?          Display this help text.\n"
          "\n"
          "USAGE (1) will stitch the whole of SRC_FILE into DST_FILE, making\n"
          "DST_FILE an exact duplicate of SRC_FILE. DST_FILE is created if it\n"
          "is missing. If DST_FILE exists it will only be overwritten if -f\n"
          "was supplied.\n"
          "\n"
          "USAGE (2) will stitch some (or all) of SRC_FILE into DST_FILE\n"
          "based on the offsets and lengths supplied. If one or more of\n"
          "SRC_OFFSET, DST_OFFSET or LENGTH is supplied then the copy will\n"
          "use FICLONERANGE. There is no force option with this mode; it is\n"
          "assumed the user wants to create DST_FILE if it is missing or\n"
          "overwrite part of it if it existed already.\n"
          "\n"
          "It is possible to emulate USAGE(1) with USAGE(2) by supplying zero\n"
          "for SRC_OFFSET, DST_OFFSET and LENGTH.\n"
          "\n",
          argv0, argv0);

  fflush(stderr);

  exit(EXIT_FAILURE);
}

/*============================================================================*/

/**
 * Parse the uint64_t value in the string @p argvN and return it if it was a
 * valid number. If it was not a valid number call print_usage_and_exit() to
 * terminate the program.
 *
 * @param[in] argvN Argument string to parse.
 * @param[in] argv0 Process name.
 * @param[in] msg   Message to log on failure. Must end with "%s" so we can
 *                  format in the failure reason.
 * @return Parsed value.
 */

static uint64_t parse_uint64 (const char *argvN,
                              const char *argv0,
                              const char *msg)
{
  char     *r_argvN = NULL;
  uintmax_t rval    = strtoumax(argvN, &r_argvN, 0);

  if (rval == UINTMAX_MAX && errno == ERANGE)
  {
    print_usage_and_exit(argv0, msg, strerror(errno));
  }
  else if (rval == 0 && r_argvN == argvN)
  {
    print_usage_and_exit(argv0, msg, "Is not a number.");
  }
  else if (rval != 0 && r_argvN[0] != '\0')
  {
    print_usage_and_exit(argv0, msg, "Contains spurious trailing characters.");
  }
  else if (rval > UINT64_MAX)
  {
    print_usage_and_exit(argv0, msg, strerror(ERANGE));
  }

  return rval;
}

/*============================================================================*/

/**
 * Parse the command-line options and fill in @p p_operation. Calls
 * print_usage_and_exit() if any errors are detected.
 */

static void parse_options (int argc, char **argv, operation_t *p_operation)
{
  AS(p_operation != NULL, "NULL p_operation pointer.");

  for (;;)
  {
    int opt = getopt(argc, argv, "acd:fl:ops:t");

    if (opt == -1)
    {
      break;
    }

    switch (opt)
    {
      case 'a':
      {
        p_operation->preserve_mode |= PRESERVE_MODE_ALL;
        break;
      }

      case 'c':
      {
        p_operation->fallback_copy = true;
        break;
      }

      case 'd':
      {
        p_operation->clone_mode = CLONE_MODE_RANGE;
        p_operation->dst_offset =
          parse_uint64(optarg, argv[0], "Failed to parse DST_OFFSET: %s");
        break;
      }

      case 'f':
      {
        p_operation->force = true;
        break;
      }

      case 'l':
      {
        p_operation->clone_mode = CLONE_MODE_RANGE;
        p_operation->src_length =
          parse_uint64(optarg, argv[0], "Failed to parse SRC_LENGTH: %s");
        break;
      }

      case 'o':
      {
        p_operation->preserve_mode |= PRESERVE_MODE_OWNER;
        break;
      }

      case 'p':
      {
        p_operation->preserve_mode |= PRESERVE_MODE_PERMS;
        break;
      }

      case 's':
      {
        p_operation->clone_mode = CLONE_MODE_RANGE;
        p_operation->src_offset =
          parse_uint64(optarg, argv[0], "Failed to parse SRC_OFFSET: %s");
        break;
      }

      case 't':
      {
        p_operation->preserve_mode |= PRESERVE_MODE_TIMES;
        break;
      }

      case '?':
      {
        print_usage_and_exit(argv[0], NULL);
        break;
      }
    }
  }

  if (optind >= argc)
  {
    print_usage_and_exit(argv[0], "Required SRC and DST filenames missing.");
  }
  else if ((argc - optind) == 1)
  {
    print_usage_and_exit(argv[0], "Required DST filename missing.");
  }

  p_operation->src_filename = argv[optind];
  p_operation->dst_filename = argv[optind + 1];

  if (p_operation->src_filename == NULL || p_operation->src_filename[0] == '\0')
  {
    print_usage_and_exit(argv[0], "Source filename is an empty string.");
  }

  if (p_operation->dst_filename == NULL || p_operation->dst_filename[0] == '\0')
  {
    print_usage_and_exit(argv[0], "Destination filename is an empty string.");
  }
}

/*============================================================================*/

/**
 * Open the source and destination files.
 */

static int open_files (operation_t *p_operation)
{
  int rc = 0;

  p_operation->src_fd = open(p_operation->src_filename, O_RDONLY);

  if (p_operation->src_fd < 0)
  {
    rc = errno;
    fprintf(stderr, "Failed to open source file \"%s\": %s\n",
            p_operation->src_filename, strerror(rc));
    return rc;
  }

  /* If cloning the whole file we try to create the destination and fail if it
   * exists (unless force was supplied). If cloning a range we don't care if
   * the file exists (we will create if needed) because we're stitching a
   * range into it.
   */
  int open_flags = O_WRONLY | O_CREAT;

  if (p_operation->clone_mode == CLONE_MODE_FILE)
  {
    open_flags |= O_EXCL;
  }

  /* Attempt to create the file with 0666 permissions. Let the user's defined
   * umask select the bits to mask out.
   */
  p_operation->dst_fd =
    open(p_operation->dst_filename, open_flags,
         S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

  rc = (p_operation->dst_fd < 0) ? errno : 0;

  if (p_operation->dst_fd < 0 && errno == EEXIST && p_operation->force)
  {
    AS(p_operation->clone_mode == CLONE_MODE_FILE,
       "Can only be here if cloning an entire file.");

    p_operation->dst_fd =
      open(p_operation->dst_filename, O_WRONLY | O_TRUNC);

    /* If force, the error from the truncating open is more interesting than
     * the create-exclusively one, which we suspected might fail. If the file
     * opened successfuly then there was no error code.
     */
    rc = (p_operation->dst_fd < 0) ? errno : 0;
  }

  if (p_operation->dst_fd < 0)
  {
    AS(rc != 0, "Error code must be non-zero at this point.");

    fprintf(stderr, "Failed to open destination file \"%s\": %s\n",
            p_operation->dst_filename, strerror(rc));
  }

  return rc;
}

/*============================================================================*/

/**
 * Close the source and destination files (if open). Safe to call
 * unconditionally.
 */

static int close_files (operation_t *p_operation)
{
  int rc = 0;

  if (p_operation->src_fd >= 0)
  {
    rc = close(p_operation->src_fd);
  }

  if (p_operation->dst_fd >= 0)
  {
    int rc2 = close(p_operation->dst_fd);
    rc = (rc == 0) ? rc2 : rc;
  }

  return rc;
}

/*============================================================================*/

/**
 * Preserve some of the attributes of the source file, if requested.
 */

static int preserve_file_attrs (operation_t *p_operation)
{
  AS(p_operation != NULL, "NULL p_operation.");
  AS(p_operation->src_fd >= 0 && p_operation->dst_fd >= 0,
     "Source (%d) or destination (%d) file handle not open.",
     p_operation->src_fd, p_operation->dst_fd);

  struct stat src_stat;
  int         rc       = fstat(p_operation->src_fd, &src_stat);

  if (rc == -1)
  {
    rc = errno;
    fprintf(stderr, "Failed to stat source file \"%s\": %s\n",
            p_operation->src_filename, strerror(rc));
  }

  if (rc == 0 && p_operation->preserve_mode & PRESERVE_MODE_OWNER)
  {
    rc = fchown(p_operation->dst_fd, src_stat.st_uid, src_stat.st_gid);

    if (rc == -1)
    {
      rc = errno;
      fprintf(stderr, "Failed to set ownership of destination file \"%s\": %s\n",
              p_operation->dst_filename, strerror(rc));
    }
  }

  if (rc == 0 && p_operation->preserve_mode & PRESERVE_MODE_TIMES)
  {
    struct timespec ts[2] = { src_stat.st_atim, src_stat.st_mtim };

    rc = futimens(p_operation->dst_fd, ts);

    if (rc == -1)
    {
      rc = errno;
      fprintf(stderr, "Failed to set timestamps on destination file \"%s\": %s\n",
              p_operation->dst_filename, strerror(rc));
    }
  }

  if (rc == 0 && p_operation->preserve_mode & PRESERVE_MODE_PERMS)
  {
    rc = fchmod(p_operation->dst_fd, src_stat.st_mode);

    if (rc == -1)
    {
      rc = errno;
      fprintf(stderr, "Failed to set mode on destination file \"%s\": %s\n",
              p_operation->dst_filename, strerror(rc));
    }
  }

  return rc;
}

/*============================================================================*/

int main (int argc, char **argv)
{
  operation_t operation =
  {
    .fallback_copy = false,
    .block_size    = 8192,
    .src_filename  = NULL,
    .dst_filename  = NULL,
    .force         = false,
    .preserve_mode = PRESERVE_MODE_DEFAULT,
    .clone_mode    = CLONE_MODE_FILE,
    .src_offset    = 0,
    .src_length    = 0,
    .dst_offset    = 0,
    .src_fd        = -1,
    .dst_fd        = -1
  };

  parse_options(argc, argv, &operation);

  int rc = open_files(&operation);

  if (rc == 0)
  {
    switch (operation.clone_mode)
    {
      case CLONE_MODE_FILE:
      {
        rc = qtm_clone_file(operation.src_fd, operation.dst_fd,
                            operation.fallback_copy, operation.block_size);
        break;
      }

      case CLONE_MODE_RANGE:
      {
        rc = qtm_clone_file_range(operation.src_fd, operation.dst_fd,
                                  operation.src_offset, operation.dst_offset,
                                  operation.src_length, operation.fallback_copy,
                                  operation.block_size);
        break;
      }
    }
  }

  if (rc == 0)
  {
    rc = preserve_file_attrs(&operation);
  }

  if (rc == 0)
  {
    rc = fsync(operation.dst_fd);

    if (rc != 0)
    {
      rc = errno;
      fprintf(stderr, "Failed to sync destination file \"%s\": %s\n",
              operation.dst_filename, strerror(rc));
    }
  }

  /* Unconditionaly close the input files. */
  int close_rc = close_files(&operation);

  if (close_rc != 0)
  {
    fprintf(stderr, "W: Error closing files: %s.\n", strerror(close_rc));
    rc = (rc == 0) ? close_rc : rc;
  }

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*============================================================================*/

