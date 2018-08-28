/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file nodefamily.c
 * \brief Code to manipulate encoded node families.
 **/

#include "core/or/or.h"
#include "feature/nodelist/nodefamily.h"
#include "feature/nodelist/nodefamily_st.h"
#include "feature/nodelist/nodelist.h"
#include "feature/relay/router.h"
#include "feature/nodelist/routerlist.h"

#include "ht.h"
#include "siphash.h"

#include "lib/container/smartlist.h"
#include "lib/ctime/di_ops.h"
#include "lib/defs/digest_sizes.h"
#include "lib/log/util_bug.h"

#include <stdlib.h>
#include <string.h>

/* We encode each member as one byte to indicate whether it's a nickname or
 * a fingerprint, plus DIGEST_LEN bytes of identity or nickname.  These
 * members are stored in sorted order.
 */
#define NODEFAMILY_MEMBER_LEN (1+DIGEST_LEN)
#define NODEFAMILY_BY_NICKNAME 0
#define NODEFAMILY_BY_RSA_ID 1

#define NF_ARRAY_SIZE(n) \
  ((n) * NODEFAMILY_MEMBER_LEN)

#define NF_MEMBER_PTR(nf, i) \
  (&((nf)->family_members[(i) * NODEFAMILY_MEMBER_LEN]))

static nodefamily_t *
nodefamily_alloc(int n_members)
{
  size_t alloc_len = offsetof(nodefamily_t, family_members) +
    NF_ARRAY_SIZE(n_members);
  nodefamily_t *nf = tor_malloc_zero(alloc_len);
  nf->n_members = n_members;
  return nf;
}

static inline unsigned int
nodefamily_hash(const nodefamily_t *nf)
{
  return (unsigned) siphash24g(nf->family_members,
                               NF_ARRAY_SIZE(nf->n_members));
}

static inline unsigned int
nodefamily_eq(const nodefamily_t *a, const nodefamily_t *b)
{
  return (a->n_members == b->n_members) &&
    fast_memeq(a->family_members, b->family_members,
               NF_ARRAY_SIZE(a->n_members));
}

static HT_HEAD(nodefamily_map, nodefamily_t) the_node_families
  = HT_INITIALIZER();

HT_PROTOTYPE(nodefamily_map, nodefamily_t, ht_ent, nodefamily_hash,
             nodefamily_eq)
HT_GENERATE2(nodefamily_map, nodefamily_t, ht_ent, nodefamily_hash,
             node_family_eq, 0.6, tor_reallocarray_, tor_free_)

nodefamily_t *
nodefamily_parse(const char *s, const uint8_t *rsa_id_self)
{
  smartlist_t *sl = smartlist_new();
  smartlist_split_string(sl, s, NULL, SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  nodefamily_t *result = nodefamily_from_members(sl, rsa_id_self);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_free(sl);
  return result;
}

static int
compare_members(const void *a, const void *b)
{
  return fast_memcmp(a, b, NODEFAMILY_MEMBER_LEN);
}

nodefamily_t *
nodefamily_from_members(const smartlist_t *members,
                        const uint8_t *rsa_id_self)
{
  int n_self = rsa_id_self ? 1 : 0;
  int n_members = smartlist_len(members) + n_self;
  nodefamily_t *tmp = nodefamily_alloc(n_members);
  SMARTLIST_FOREACH_BEGIN(members, const char *, cp) {
    uint8_t *ptr = NF_MEMBER_PTR(tmp, cp_sl_idx);
    if (is_legal_nickname(cp)) {
      ptr[0] = NODEFAMILY_BY_NICKNAME;
      tor_assert(strlen(cp) < DIGEST_LEN); // guaranteed by is_legal_nickname
      memcpy(ptr+1, cp, strlen(cp));
    } else if (is_legal_hexdigest(cp)) {
      char digest_buf[DIGEST_LEN];
      char nn_buf[MAX_NICKNAME_LEN+1];
      char nn_char=0;
      if (hex_digest_nickname_decode(cp, digest_buf, &nn_char, nn_buf)<0)
        goto err;
      ptr[0] = NODEFAMILY_BY_RSA_ID;
      memcpy(ptr+1, digest_buf, DIGEST_LEN);
    } else {
      goto err;
    }
  } SMARTLIST_FOREACH_END(cp);

  if (rsa_id_self) {
    /* Add self. */
    uint8_t *ptr = NF_MEMBER_PTR(tmp, smartlist_len(members));
    ptr[0] = NODEFAMILY_BY_RSA_ID;
    memcpy(ptr+1, rsa_id_self, DIGEST_LEN);
  }

  /* Sort tmp into canonical order. */
  qsort(tmp->family_members, n_members, NODEFAMILY_MEMBER_LEN,
        compare_members);

  /* Remove duplicates. */
  int i;
  for (i = 0; i < n_members-1; ++i) {
    uint8_t *thisptr = NF_MEMBER_PTR(tmp, i);
    uint8_t *nextptr = NF_MEMBER_PTR(tmp, i+1);
    if (fast_memeq(thisptr, nextptr, NODEFAMILY_MEMBER_LEN)) {
      memmove(thisptr, nextptr, (n_members-i-1)*NODEFAMILY_MEMBER_LEN);
      --n_members;
      --i;
    }
  }
  int n_members_alloc = tmp->n_members;
  tmp->n_members = n_members;

  nodefamily_t *found = HT_FIND(nodefamily_map, &the_node_families, tmp);
  if (found) {
    ++found->refcnt;
    tor_free(tmp);
    return found;
  } else {
    if (n_members_alloc != n_members) {
      /* Compact the family if needed */
      nodefamily_t *tmp2 = nodefamily_alloc(n_members);
      memcpy(tmp2->family_members, tmp->family_members,
             n_members * NODEFAMILY_MEMBER_LEN);
      tor_free(tmp);
      tmp = tmp2;
    }

    tmp->refcnt = 1;
    HT_INSERT(nodefamily_map, &the_node_families, tmp);
    return tmp;
  }

 err:
  tor_free(tmp);
  return NULL;
}

void
nodefamily_free_(nodefamily_t *family)
{
  if (family == NULL)
    return;

  --family->refcnt;

  if (family->refcnt == 0) {
    HT_REMOVE(nodefamily_map, &the_node_families, family);
    tor_free(family);
  }
}


bool
nodefamily_contains_rsa_id(const nodefamily_t *family,
                           const uint8_t *rsa_id)
{
  if (family == NULL)
    return false;

  unsigned i;
  for (i = 0; i < family->n_members; ++i) {
    const uint8_t *ptr = NF_MEMBER_PTR(family, i);
    if (ptr[0] == NODEFAMILY_BY_RSA_ID &&
        fast_memeq(ptr+1, rsa_id, DIGEST_LEN)) {
      return true;
    }
  }
  return false;
}

bool
nodefamily_contains_nickname(const nodefamily_t *family,
                             const char *name)
{
  if (family == NULL)
    return false;

  unsigned i;
  for (i = 0; i < family->n_members; ++i) {
    const uint8_t *ptr = NF_MEMBER_PTR(family, i);
    // note that the strcmp() is safe because there is always at least one
    // NUL in the encoded nickname, because all legal nicknames less than
    // DIGEST_LEN bytes long.
    if (ptr[0] == NODEFAMILY_BY_NICKNAME && !strcmp((char*)ptr+1, name)) {
      return true;
    }
  }
  return false;
}

bool
nodefamily_contains_node(const nodefamily_t *family,
                         const node_t *node)
{
  return
    nodefamily_contains_nickname(family, node_get_nickname(node))
    ||
    nodefamily_contains_rsa_id(family, node_get_rsa_id_digest(node));
}

void
nodefamily_add_nodes_to_smartlist(const nodefamily_t *family,
                                  smartlist_t *out)
{
  if (!family)
    return;

  unsigned i;
  for (i = 0; i < family->n_members; ++i) {
    const uint8_t *ptr = NF_MEMBER_PTR(family, i);
    const node_t *node = NULL;
    switch (ptr[0]) {
      case NODEFAMILY_BY_NICKNAME:
        node = node_get_by_nickname((char*)ptr+1, NNF_NO_WARN_UNNAMED);
        break;
      case NODEFAMILY_BY_RSA_ID:
        node = node_get_by_id((char*)ptr+1);
        break;
      default:
        tor_assert_nonfatal_unreached();
        break;
    }
    if (node)
      smartlist_add(out, (void *)node);
  }
}

char *
nodefamily_format(const nodefamily_t *family)
{
  if (!family)
    return tor_strdup("");

  unsigned i;
  smartlist_t *sl = smartlist_new();
  for (i = 0; i < family->n_members; ++i) {
    const uint8_t *ptr = NF_MEMBER_PTR(family, i);
    switch (ptr[0]) {
      case NODEFAMILY_BY_NICKNAME:
        smartlist_add_strdup(sl, (char*)ptr+1);
        break;
      case NODEFAMILY_BY_RSA_ID: {
        char buf[HEX_DIGEST_LEN+2];
        buf[0]='$';
        base16_encode(buf+1, sizeof(buf)-1, (char*)ptr+1, DIGEST_LEN);
        smartlist_add_strdup(sl, buf);
        break;
      }
      default:
        tor_assert_nonfatal_unreached();
        break;
    }
  }

  char *result = smartlist_join_strings(sl, " ", 0, NULL);
  SMARTLIST_FOREACH(sl, char *, cp, tor_free(cp));
  smartlist_free(sl);
  return result;
}

void
nodefamily_free_all(void)
{
  HT_CLEAR(nodefamily_map, &the_node_families);
}
