/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef TOR_MAPSTORE_H
#define TOR_MAPSTORE_H

#include "lib/malloc/malloc.h"

typedef struct tor_mapstore_t tor_mapstore_t;
struct tor_mmap_t;
struct smartlist_t;
typedef struct tor_mapstore_item_t {
  size_t offset;
  unsigned in_journal : 1;
  unsigned len : 31;
} tor_mapstore_item_t;

mapstore_t *tor_mapstore_open(const char *fname,
                              const char *fname_journal);
void tor_mapstore_free_(tor_mapstore_t *);
#define tor_mapstore_free(p) \
  FREE_AND_NULL(tor_mapstore_t, tor_mapstore_free_, (p))
const struct tor_mmap_t *tor_mapstore_get_map(const tor_mapstore_t *store,
                                              int get_journal);
int tor_mapstore_append(const tor_mapstore_t *store,
                        const char *data,
                        size_t datalen,
                        tor_mapstore_item_t *out);
int tor_mapstore_rebuild(tor_mapstore_t *store,
                         struct smartlist_t *items);

const char *tor_mapstore_item_get(tor_mapstore_item_t *item, size_t *sz_out);

#ifdef MAPSTORE_PRIVATE
struct tor_mapstore_t {
  const char *fname;
  const char *fname_journal;

  struct tor_mmap_t *map;
  struct tor_mmap_t *map_journal;
};
#endif

#endif /* TOR_MAPSTORE_H */
