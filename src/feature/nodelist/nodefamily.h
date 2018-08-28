/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file nodefamily.h
 * \brief Header file for nodefamily.c.
 **/

#ifndef TOR_NODEFAMILY_H
#define TOR_NODEFAMILY_H

#include "lib/malloc/malloc.h"
#include <stdbool.h>

typedef struct nodefamily_t nodefamily_t;
struct node_t;
struct smartlist_t;

nodefamily_t *nodefamily_parse(const char *s, const uint8_t *rsa_id_self);
nodefamily_t *nodefamily_from_members(const struct smartlist_t *members,
                                      const uint8_t *rsa_id_self);
void nodefamily_free_(nodefamily_t *family);
#define nodefamily_free(familiy) \
  FREE_AND_NULL(nodefamily_t, nodefamily_free_, (family))

bool nodefamily_contains_rsa_id(const nodefamily_t *family,
                                const uint8_t *rsa_id);
bool nodefamily_contains_nickname(const nodefamily_t *family,
                                  const char *name);
bool nodefamily_contains_node(const nodefamily_t *family,
                              const struct node_t *node);
void nodefamily_add_nodes_to_smartlist(const nodefamily_t *family,
                                       struct smartlist_t *out);
char *nodefamily_format(const nodefamily_t *family);

void nodefamily_free_all(void);

#endif
