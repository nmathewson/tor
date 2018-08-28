/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#define MAPSTORE_PRIVATE
#include "lib/fs/mapstore.h"
#include "lib/container/smartlist.h"
#include "lib/fs/mmap.h"

mapstore_t *
tor_mapstore_open(const char *fname,
                  const char *fname_journal)
{
  tor_mapstore_t *result = tor_malloc_zero(sizeof(tor_mapstore_t));
  result->fname = tor_strdup(fname);
  result->fname_journal = tor_strdup(fname_journal);
  result->map = tor_mmap_file(fname);
  if (!result->map)
    goto err;
  result->map_journal = tor_mmap_file(fname_journal);
  if (!result->map_journal)
    goto err;

  return result;
 err:
  tor_mapstore_free(result);
  return NULL;
}

void
tor_mapstore_free_(tor_mapstore_t *store)
{
  if (!store)
    return;
  tor_free(store->fname);
  tor_free(store->fname_journal);
  tor_munmap_file(store->map);
  tor_munmap_file(store->map_journal);
  tor_free(store);
}

const struct tor_mmap_t *
tor_mapstore_get_map(const tor_mapstore_t *store,
                     int get_journal)
{
  return get_journal ? store->map_journal : store->map;
}
int
tor_mapstore_append(const tor_mapstore_t *store,
                    const char *data,
                    size_t datalen,
                    tor_mapstore_item_t *out)
{
  (void)store;
  (void)data;
  (void)datalen;
  (void)out;
  return -1;
}
int
tor_mapstore_rebuild(tor_mapstore_t *store,
                     struct smartlist_t *items);
{
  (void)store;
  (void)items;
  return -1;
}
