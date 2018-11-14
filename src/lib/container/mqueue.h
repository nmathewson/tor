/* Copyright (c) 2003-2004, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2018, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#ifndef TOR_MQUEUE_H
#define TOR_MQUEUE_H

/**
 * \file mqueue.h
 *
 * \brief Header for mqueue.c
 **/

#include <stddef.h>

struct smartlist_t;
/**
 * A message queue, implemented as a ring buffer.
 */
typedef struct mqueue_t {
  /** A ring buffer of size <b>capacity</b>. */
  void **members;
  /** The amount of items that can be stored in <b>members</b>. */
  size_t capacity;
  /** The index of the head of the queue within members. */
  size_t head;
  /** The index one past the last item of the queue within members; if this is
   * equal to "first" then the array is empty. */
  size_t tail;
} mqueue_t;

void mqueue_init(mqueue_t *mq);
void mqueue_clear(mqueue_t *mq);
void mqueue_foreach(mqueue_t *mq, void (*fn)(void *, void *),
                    void *userarg);
size_t mqueue_len(const mqueue_t *mq);
void mqueue_push(mqueue_t *mq, void *item);
void *mqueue_pop(mqueue_t *mq);

#endif /* !defined(TOR_MQUEUE_H) */
