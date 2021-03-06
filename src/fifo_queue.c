/*
 * GeeXboX Valhalla: tiny media scanner API.
 * Copyright (C) 2008-2009 Mathieu Schroeter <mathieu@schroetersa.ch>
 *
 * This file is part of libvalhalla.
 *
 * libvalhalla is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libvalhalla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libvalhalla; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

#include "fifo_queue.h"

typedef struct fifo_queue_item_s {
  int id;
  void *data;
  struct fifo_queue_item_s *next;
} fifo_queue_item_t;

struct fifo_queue_s {
  fifo_queue_item_t *item;
  fifo_queue_item_t *item_last;
  pthread_mutex_t mutex;
  sem_t sem;
};


fifo_queue_t *
vh_fifo_queue_new (void)
{
  fifo_queue_t *queue;

  queue = calloc (1, sizeof (fifo_queue_t));
  if (!queue)
    return NULL;

  pthread_mutex_init (&queue->mutex, NULL);
  sem_init (&queue->sem, 0, 0);

  return queue;
}

void
vh_fifo_queue_free (fifo_queue_t *queue)
{
  fifo_queue_item_t *item, *next;

  if (!queue)
    return;

  item = queue->item;
  while (item)
  {
    next = item->next;
    free (item);
    item = next;
  }

  pthread_mutex_destroy (&queue->mutex);
  sem_destroy (&queue->sem);

  free (queue);
}

int
vh_fifo_queue_push (fifo_queue_t *queue,
                    fifo_queue_prio_t p, int id, void *data)
{
  fifo_queue_item_t *item;

  if (!queue)
    return FIFO_QUEUE_ERROR_QUEUE;

  pthread_mutex_lock (&queue->mutex);

  item = queue->item;
  if (item)
  {
    switch (p)
    {
    default:
    case FIFO_QUEUE_PRIORITY_NORMAL:
      queue->item_last->next = calloc (1, sizeof (fifo_queue_item_t));
      item = queue->item_last->next;
      if (item)
        queue->item_last = item;
      break;

    case FIFO_QUEUE_PRIORITY_HIGH:
      item = calloc (1, sizeof (fifo_queue_item_t));
      if (!item)
        break;
      item->next = queue->item;
      queue->item = item;
      break;
    }
  }
  else
  {
    item = calloc (1, sizeof (fifo_queue_item_t));
    queue->item = item;
    queue->item_last = item;
  }

  if (!item)
  {
    pthread_mutex_unlock (&queue->mutex);
    return FIFO_QUEUE_ERROR_MALLOC;
  }

  item->id = id;
  item->data = data;

  /* new entry in the queue is ok */
  sem_post (&queue->sem);

  pthread_mutex_unlock (&queue->mutex);

  return FIFO_QUEUE_SUCCESS;
}

int
vh_fifo_queue_pop (fifo_queue_t *queue, int *id, void **data)
{
  fifo_queue_item_t *item, *next;

  if (!queue)
    return FIFO_QUEUE_ERROR_QUEUE;

  /* wait on the queue */
  sem_wait (&queue->sem);

  pthread_mutex_lock (&queue->mutex);
  item = queue->item;
  if (!item)
  {
    pthread_mutex_unlock (&queue->mutex);
    return FIFO_QUEUE_ERROR_EMPTY;
  }

  if (id)
    *id = item->id;
  if (data)
    *data = item->data;

  /* remove the entry and go to the next */
  next = item->next;
  free (item);
  queue->item = next;
  pthread_mutex_unlock (&queue->mutex);

  return FIFO_QUEUE_SUCCESS;
}

void *
vh_fifo_queue_search (fifo_queue_t *queue, int *id, const void *tocmp,
                      int (*cmp_fct) (const void *tocmp,
                                      int id, const void *data))
{
  void *data = NULL;
  fifo_queue_item_t *item;

  if (!queue || !tocmp || !cmp_fct)
    return NULL;

  pthread_mutex_lock (&queue->mutex);

  for (item = queue->item; item; item = item->next)
    if (!cmp_fct (tocmp, item->id, item->data))
    {
      *id  = item->id;
      data = item->data;
      break;
    }

  pthread_mutex_unlock (&queue->mutex);

  return data;
}

void
vh_fifo_queue_moveup (fifo_queue_t *queue, const void *tomove,
                      int (*cmp_fct) (const void *tocmp,
                                      int id, const void *data))
{
  fifo_queue_item_t *item, *item_p = NULL;

  if (!queue || !tomove || !cmp_fct)
    return;

  pthread_mutex_lock (&queue->mutex);

  for (item = queue->item; item; item = item->next)
  {
    if (!cmp_fct (tomove, item->id, item->data))
    {
      if (!item_p)
        break;
      item_p->next = item->next;
      if (!item_p->next)
        queue->item_last = item_p;
      item->next = queue->item;
      queue->item = item;
      break;
    }
    item_p = item;
  }

  pthread_mutex_unlock (&queue->mutex);
}
