/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file mqueue.c
 *
 * \brief Message-queue structure based on a ring buffer.
 **/

#include "orconfig.h"
#include "lib/malloc/malloc.h"
#include "lib/container/mqueue.h"
#include "lib/container/smartlist.h"
#include "lib/log/util_bug.h"

#include <string.h>

#define INITIAL_SIZE 16

/** Initialize a mqueue_t structure. */
void
mqueue_init(mqueue_t *mq)
{
  memset(mq, 0, sizeof(*mq));
  mq->capacity = INITIAL_SIZE;
  mq->len = 0;
  mq->first = 0;
  mq->members = tor_calloc(INITIAL_SIZE, sizeof(void*));
}

/** Clear a mqueue_t structure.  Does not free the items held in the queue. */
void
mqueue_clear(mqueue_t *mq)
{
  tor_free(mq->members);
  memset(mq, 0, sizeof(*mq));
}

/** Run <b>fn</b> on every element of <b>mq</b>, passing it <b>arg</b> as a
 * second argument.
 *
 * Items are processed from the front of the queue to the end.
 **/
void
mqueue_foreach(mqueue_t *mq, void (*fn)(void *, void *), void *userarg)
{
  for (size_t idx = mq->first, n = 0; n < mq->len;  ++idx, ++n) {
    if (idx >= mq->capacity)
      idx = 0;
    fn(mq->members[idx], userarg);
  }

}

/** Return the number of elements stored in <b>mq</b>. */
size_t
mqueue_len(const mqueue_t *mq)
{
  return mq->len;
}

static void
mqueue_expand(mqueue_t *mq)
{
  size_t new_capacity = (mq->capacity * 2);

  // Overflow should be impossible, but let's make sure.
  tor_assert(new_capacity > mq->capacity);

  void **new_members =
    tor_reallocarray(mq->members, new_capacity, sizeof(void*));

  if (mq->capacity - mq->first > mq->len) {
    /* The ring buffer was wrapped around the end; we need to
     * move the elements that were at the end of the array.
     */
    size_t delta = (new_capacity - mq->capacity); // == capacity
    memmove(&new_members[mq->first + delta],
            &new_members[mq->first],
            (mq->capacity - mq->first) * sizeof(void*));
    mq->first += delta;
  }
  mq->members = new_members;
  mq->capacity = new_capacity;
}

/** Append <b>item</b> to the end of <b>mq</b>. */
void
mqueue_push(mqueue_t *mq, void *item)
{
  if (mq->len == mq->capacity) {
    mqueue_expand(mq);
  }

  size_t idx;
#if 0
  /* We'd like to do it this way, but that would potentially overflow. */
  idx = mq->first + mq->len;
  if (idx >= mq->capacity) {
    idx -= mq->capacity;
  }
#endif
  if (mq->capacity - mq->first < mq->len) {
    idx = mq->first - (mq->capacity - mq->len);
  } else {
    idx = mq->first + mq->len;
  }

  mq->members[idx] = item;
  ++mq->len;
}

/** Remove and return the first item in <b>mq</b>.  Return NULL if <b>mq</b>
 * is empty. */
void *
mqueue_pop(mqueue_t *mq)
{
  if (mq->len == 0)
    return NULL;

  void *result = mq->members[mq->first];
  mq->members[mq->first] = NULL; // Clear the pointer, for safety.
  ++mq->first;
  if (mq->first == mq->capacity)
    mq->first = 0;
  --mq->len;
  return result;
}
