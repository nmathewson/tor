/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef TOR_NODEFAMILY_ST_H
#define TOR_NODEFAMILY_ST_H

#include "orconfig.h"
#include "ht.h"

struct nodefamily_t {
  HT_ENTRY(nodefamily_t) ht_ent;
  uint32_t refcnt;
  uint32_t n_members;
  uint8_t family_members[FLEXIBLE_ARRAY_MEMBER];
};

#endif
