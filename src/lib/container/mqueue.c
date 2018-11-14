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
#include <stdio.h>

#define INITIAL_SIZE 16

/** Initialize a mqueue_t structure. */
void
mqueue_init(mqueue_t *mq)
{
  memset(mq, 0, sizeof(*mq));
  mq->capacity = INITIAL_SIZE;
  mq->head = 0;
  mq->tail = 0;
  mq->members = tor_calloc(INITIAL_SIZE, sizeof(void*));
}

/** Clear a mqueue_t structure.  Does not free the items held in the queue. */
void
mqueue_clear(mqueue_t *mq)
{
  tor_free(mq->members);
  memset(mq, 0, sizeof(*mq));
}

/** Return true iff mq has no elements. */
static inline bool
mqueue_empty(const mqueue_t *mq)
{
  return mq->head == mq->tail;
}

/** Return the next index in <b>mq</b> after <b>idx</b>, wrapping around
 * if necessary */
static inline size_t
next_idx(const mqueue_t *mq, size_t idx)
{
  ++idx;
  if (idx == mq->capacity)
    return 0;
  else
    return idx;
}

/** Run <b>fn</b> on every element of <b>mq</b>, passing it <b>arg</b> as a
 * second argument.
 *
 * Items are processed from the front of the queue to the end.
 **/
void
mqueue_foreach(mqueue_t *mq, void (*fn)(void *, void *), void *userarg)
{
  for (size_t idx = mq->head; idx != mq->tail; idx = next_idx(mq, idx)) {
    fn(mq->members[idx], userarg);
  }
}

/** Return the number of elements stored in <b>mq</b>. */
size_t
mqueue_len(const mqueue_t *mq)
{
  if (mq->head <= mq->tail)
    return mq->tail - mq->head;
  else
    return (mq->capacity - mq->head) + mq->tail;
}

static void
mqueue_expand(mqueue_t *mq)
{
  size_t new_capacity = (mq->capacity * 2);

  // Overflow should be impossible, but let's make sure.
  tor_assert(new_capacity > mq->capacity);

  void **new_members =
    tor_reallocarray(mq->members, new_capacity, sizeof(void*));

  if (mq->tail < mq->head) {
    /* The ring buffer was wrapped around the end; we need to
     * move the elements that were at the end of the array.
     */
    size_t delta = (new_capacity - mq->capacity); // == capacity
    memmove(&new_members[mq->head + delta],
            &new_members[mq->head],
            (mq->capacity - mq->head) * sizeof(void*));
    mq->head += delta;
  }
  mq->members = new_members;
  mq->capacity = new_capacity;
}

/** Append <b>item</b> to the end of <b>mq</b>. */
void
mqueue_push(mqueue_t *mq, void *item)
{
  if (next_idx(mq, mq->tail) == mq->head) {
    mqueue_expand(mq);
  }

  mq->members[mq->tail] = item;
  mq->tail = next_idx(mq, mq->tail);
}

/** Remove and return the first item in <b>mq</b>.  Return NULL if <b>mq</b>
 * is empty. */
void *
mqueue_pop(mqueue_t *mq)
{
  if (mq->head == mq->tail)
    return NULL;

  void *result = mq->members[mq->head];
  mq->members[mq->head] = NULL; // Clear the pointer, for safety.
  mq->head = next_idx(mq, mq->head);

  return result;
}
