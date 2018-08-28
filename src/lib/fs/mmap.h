/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file mmap.h
 *
 * \brief Header for mmap.c
 **/

#ifndef TOR_MMAP_H
#define TOR_MMAP_H

#include "lib/cc/compat_compiler.h"
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windef.h>
#endif

/** Represents an mmaped file. Allocated via tor_mmap_file; freed with
 * tor_munmap_file. */
typedef struct tor_mmap_t {
  const char *data; /**< Mapping of the file's contents. */
  size_t size; /**< Size of the file. */

  /* None of the fields below should be accessed from outside mmap.c */
  struct {
#ifdef HAVE_MMAP
    size_t mapping_size; /**< Size of the actual mapping. (This is the
                          * original file size, rounded up to the nearest
                          * page.) */
    int fd; /**< File descriptor for the underlying file, if this file
             * is append-able */
#elif defined _WIN32
    HANDLE mmap_handle; /**< Handle for the actual mapping object */
    HANDLE file_handle; /**< File handle for the underlying file, if this file
                         * is append-able */
#endif /* defined(HAVE_MMAP) || ... */
    unsigned int is_dirty; /**< Have we appended to this file without
                            * remapping? */
  } map_private;

} tor_mmap_t;

#define TOR_MMAP_APPEND_OK (1u<<1)

tor_mmap_t *tor_mmap_file(const char *filename, unsigned flags);
int tor_munmap_file(tor_mmap_t *handle);
int tor_mmap_append(tor_mmap_t *mapping,
                    const char *data,
                    size_t len,
                    off_t *offset_out);
int tor_mremap(tor_mmap_t *mapping);

#endif
