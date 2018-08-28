/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file mmap.c
 *
 * \brief Cross-platform support for mapping files into our address space.
 **/

#include "lib/fs/mmap.h"
#include "lib/fs/files.h"
#include "lib/fdio/fdio.h"
#include "lib/log/log.h"
#include "lib/log/util_bug.h"
#include "lib/log/win32err.h"
#include "lib/string/compat_string.h"
#include "lib/malloc/malloc.h"

#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <errno.h>
#include <string.h>
#include <stdbool.h>

#if defined(HAVE_MMAP) || defined(RUNNING_DOXYGEN)
static char *
mmap_fd(int fd, size_t *size_out)
{
  struct stat st;

  /* Get the size of the file */
  int result = fstat(fd, &st);
  if (result != 0) {
    int save_errno = errno;
    log_warn(LD_FS,
             "Couldn't fstat opened descriptor during mmap: %s",
              strerror(errno));
    close(fd);
    errno = save_errno;
    return NULL;
  }
  size_t size = (size_t)(st.st_size);

  if (st.st_size > SSIZE_T_CEILING || (off_t)size < st.st_size) {
    log_warn(LD_FS, "File is too large to mmap. Ignoring.");
    errno = EFBIG;
    close(fd);
    return NULL;
  }
  if (!size) {
    /* Zero-length file. If we call mmap on it, it will succeed but
     * return NULL, and bad things will happen. So just fail. */
    log_info(LD_FS,"File is empty. Ignoring.");
    errno = ERANGE;
    close(fd);
    return NULL;
  }

  *size_out = size;
  return mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
}

/** Try to create a memory mapping for <b>filename</b> and return it.  On
 * failure, return NULL. Sets errno properly, using ERANGE to mean
 * "empty file". Must only be called on trusted Tor-owned files, as changing
 * the underlying file's size causes unspecified behavior. */
tor_mmap_t *
tor_mmap_file(const char *filename, unsigned flags)
{
  (void) flags;
  int fd; /* router file */
  char *string;
  tor_mmap_t *res;
  size_t size, filesize;
  const bool append_ok = !! (flags & TOR_MMAP_APPEND_OK);

  tor_assert(filename);

  {
    unsigned open_flags;
    if (append_ok) {
      open_flags = O_RDWR|O_CREAT|O_APPEND;
    } else {
      open_flags = O_RDONLY;
    }
    fd = tor_open_cloexec(filename, open_flags, 0600);
  }
  if (fd<0) {
    int save_errno = errno;
    int severity = (errno == ENOENT) ? LOG_INFO : LOG_WARN;
    log_fn(severity, LD_FS,"Could not open \"%s\" for mmap(): %s",filename,
           strerror(errno));
    errno = save_errno;
    return NULL;
  }

  string = mmap_fd(fd, &size);
  filesize = size;

  if (!append_ok) {
    close(fd);
    fd = -1;
  }
  if (string == MAP_FAILED) {
    int save_errno = errno;
    log_warn(LD_FS,"Could not mmap file \"%s\": %s", filename,
             strerror(errno));
    errno = save_errno;
    if (fd >= 0)
      close(fd);
    return NULL;
  }
  if (fd)
    tor_fd_seekend(fd);

  res = tor_malloc_zero(sizeof(tor_mmap_t));
  res->data = string;
  res->size = filesize;
  res->map_private.fd = fd;
  res->map_private.mapping_size = size;
  res->map_private.is_dirty = 0;

  return res;
}

/**DOCDOC*/
int
tor_mmap_append(tor_mmap_t *mapping,
                const char *data,
                size_t len,
                off_t *offset_out)
{
  tor_assert(offset_out);
  const int fd = mapping->map_private.fd;

  if (BUG(fd == -1))
    return -1; // We can't append to a no-appending file.

  mapping->map_private.is_dirty = 1;

  off_t pos = tor_fd_getpos(fd);
  if (write_all_to_fd(fd, data, len) != (ssize_t)len) {
    log_warn(LD_GENERAL, "Error while appending to mapped file: %s",
             strerror(errno));
    tor_fd_setpos(fd, pos);
    if (ftruncate(fd, pos) < 0) {
      log_warn(LD_GENERAL, "Error while truncating to mapped file: %s",
               strerror(errno));
    }
    *offset_out = 0;
    return -1;
  }

  *offset_out = pos;
  return 0;
}

/**DOCDOC*/
int
tor_mremap(tor_mmap_t *handle)
{
  if (! handle->map_private.is_dirty)
    return 0;

  char *newmap;
  size_t newsize=0;

  newmap = mmap_fd(handle->map_private.fd, &newsize);
  if (newmap == MAP_FAILED)
    return -1;

  munmap((char*)handle->data, handle->map_private.mapping_size);

  handle->data = newmap;
  handle->size = newsize;
  handle->map_private.mapping_size = newsize;
  handle->map_private.is_dirty = 0;

  return 0;
}

/** Release storage held for a memory mapping; returns 0 on success,
 * or -1 on failure (and logs a warning). */
int
tor_munmap_file(tor_mmap_t *handle)
{
  int res;

  if (handle == NULL)
    return 0;

  res = munmap((char*)handle->data, handle->map_private.mapping_size);
  if (res == 0) {
    /* munmap() succeeded */
    tor_free(handle);
  } else {
    log_warn(LD_FS, "Failed to munmap() in tor_munmap_file(): %s",
             strerror(errno));
    res = -1;
  }

  return res;
}
#elif defined(_WIN32)
tor_mmap_t *
tor_mmap_file(const char *filename, unsigned flags)
{
  (void)flags;
  TCHAR tfilename[MAX_PATH]= {0};
  tor_mmap_t *res = tor_malloc_zero(sizeof(tor_mmap_t));
  int empty = 0;
  HANDLE file_handle = INVALID_HANDLE_VALUE;
  DWORD size_low, size_high;
  uint64_t real_size;
  const bool append_ok = !! (flags & TOR_MMAP_APPEND_OK);
  res->map_private.file_handle = NULL;
  res->map_private.mmap_handle = NULL;
  res->map_private.is_dirty = 0;
#ifdef UNICODE
  mbstowcs(tfilename,filename,MAX_PATH);
#else
  strlcpy(tfilename,filename,MAX_PATH);
#endif
  const DWORD desired_access =
    append_ok ? GENERIC_READ|GENERIC_WRITE : GENERIC_READ;
  const DWORD creation_disposition =
    append_ok ? CREATE_NEW : OPEN_EXISTING;
  file_handle = CreateFile(tfilename,
                           desired_accdess, FILE_SHARE_READ,
                           NULL,
                           creation_disposition,
                           FILE_ATTRIBUTE_NORMAL,
                           0);

  if (file_handle == INVALID_HANDLE_VALUE)
    goto win_err;

  size_low = GetFileSize(file_handle, &size_high);

  if (size_low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
    log_warn(LD_FS,"Error getting size of \"%s\".",filename);
    goto win_err;
  }
  if (size_low == 0 && size_high == 0) {
    log_info(LD_FS,"File \"%s\" is empty. Ignoring.",filename);
    empty = 1;
    goto err;
  }
  real_size = (((uint64_t)size_high)<<32) | size_low;
  if (real_size > SIZE_MAX) {
    log_warn(LD_FS,"File \"%s\" is too big to map; not trying.",filename);
    goto err;
  }
  res->size = real_size;

  res->map_private.mmap_handle = CreateFileMapping(file_handle,
                                       NULL,
                                       PAGE_READONLY,
                                       size_high,
                                       size_low,
                                       NULL);
  if (res->map_private.mmap_handle == NULL)
    goto win_err;
  res->data = (char*) MapViewOfFile(res->map_private.mmap_handle,
                                    FILE_MAP_READ,
                                    0, 0, 0);
  if (!res->data)
    goto win_err;
  if (append_ok) {
    DWORD hi = 0;
    SetFilePointer(file_handle, 0, &hi, FILE_END);
    res->map_private.file_handle = file_handle;
    file_handle = INVALID_HANDLE_VALUE;
  }

  if (file_handle != INVALID_HANDLE_VALUE)
    CloseHandle(file_handle);
  return res;
 win_err: {
    DWORD e = GetLastError();
    int severity = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ?
      LOG_INFO : LOG_WARN;
    char *msg = format_win32_error(e);
    log_fn(severity, LD_FS, "Couldn't mmap file \"%s\": %s", filename, msg);
    tor_free(msg);
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
      errno = ENOENT;
    else
      errno = EINVAL;
  }
 err:
  if (empty)
    errno = ERANGE;
  if (file_handle != INVALID_HANDLE_VALUE)
    CloseHandle(file_handle);
  tor_munmap_file(res);
  return NULL;
}

/**DOCDOC*/
int
tor_mmap_append(tor_mmap_t *mapping,
                const char *data,
                size_t len,
                off_t *offset_out)
{
  tor_assert(offset_out);
  const HANDLE handle = mapping->map_private.file_handle

  if (BUG(handle == INVALID_HANDLE_VALUE))
    return -1; // We can't append to a no-appending file.

  mapping->map_private.is_dirty = 1;

  DWORD lo,hi=0;
  lo = SetFilePointer(handle, 0, &hi, FILE_CURRENT); // Query current pos.
  const off_t pos = lo | (hi << 32);

  DWORD writen = 0;
  if (! WriteFile(handle, data, len, &written, NULL) || written != len) {
    log_warn(LD_GENERAL, "Error while appending to mapped file: %s",
             strerror(errno));
    SetFilePointer(handle, lo, &hi, FILE_BEGIN);
    SetEndOfFile(handle);
    *offset_out = 0;
    return -1;
  }

  *offset_out = pos;
  return 0;
}

/**DOCDOC*/
int
tor_mremap(tor_mmap_t *mapping)
{
  if (! mapping->map_private.is_dirty)
    return 0;

  return 0;
}

/* Unmap the file, and return 0 for success or -1 for failure */
int
tor_munmap_file(tor_mmap_t *handle)
{
  if (handle == NULL)
    return 0;

  if (handle->data) {
    /* This is an ugly cast, but without it, "data" in struct tor_mmap_t would
       have to be redefined as non-const. */
    BOOL ok = UnmapViewOfFile( (LPVOID) handle->data);
    if (!ok) {
      log_warn(LD_FS, "Failed to UnmapViewOfFile() in tor_munmap_file(): %d",
               (int)GetLastError());
    }
  }

  if (handle->map_private.mmap_handle != NULL)
    CloseHandle(handle->map_private.mmap_handle);
  tor_free(handle);

  return 0;
}
#else
#error "cannot implement tor_mmap_file"
#endif /* defined(HAVE_MMAP) || ... || ... */
