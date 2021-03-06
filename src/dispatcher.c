/*
 * GeeXboX Valhalla: tiny media scanner API.
 * Copyright (C) 2009 Mathieu Schroeter <mathieu@schroetersa.ch>
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
#include <string.h>

#include "valhalla.h"
#include "valhalla_internals.h"
#include "utils.h"
#include "fifo_queue.h"
#include "logs.h"
#include "thread_utils.h"
#include "parser.h"
#include "dbmanager.h"
#include "dispatcher.h"

#ifdef USE_GRABBER
#include "grabber.h"
#include "downloader.h"
#endif /* USE_GRABBER */

#define VH_HANDLE dispatcher->valhalla

struct dispatcher_s {
  valhalla_t   *valhalla;
  pthread_t     thread;
  fifo_queue_t *fifo;
  int           priority;

  int             wait;
  int             run;
  pthread_mutex_t mutex_run;

  VH_THREAD_PAUSE_ATTRS
};


static inline int
dispatcher_is_stopped (dispatcher_t *dispatcher)
{
  int run;
  pthread_mutex_lock (&dispatcher->mutex_run);
  run = dispatcher->run;
  pthread_mutex_unlock (&dispatcher->mutex_run);
  return !run;
}

static void *
dispatcher_thread (void *arg)
{
  int res, tid;
  int e;
  void *data = NULL;
  file_data_t *pdata;
  dispatcher_t *dispatcher = arg;

  struct dispatcher_step_s {
    void *handler;
    void (*fct) (void *handler, fifo_queue_prio_t prio, int action, void *data);
  } send[] = {
    [STEP_PARSING]      = { NULL, (void *) vh_parser_action_send     },
#ifdef USE_GRABBER
    [STEP_GRABBING]     = { NULL, (void *) vh_grabber_action_send    },
    [STEP_DOWNLOADING]  = { NULL, (void *) vh_downloader_action_send },
#endif /* USE_GRABBER */
    [STEP_ENDING]       = { NULL, (void *) vh_dbmanager_action_send  },
  };


  if (!dispatcher)
    pthread_exit (NULL);

  tid = vh_setpriority (dispatcher->priority);

  vh_log (VALHALLA_MSG_VERBOSE,
          "[%s] tid: %i priority: %i", __FUNCTION__, tid, dispatcher->priority);

  send[STEP_PARSING].handler     = VH_HANDLE->parser;
#ifdef USE_GRABBER
  send[STEP_GRABBING].handler    = VH_HANDLE->grabber;
  send[STEP_DOWNLOADING].handler = VH_HANDLE->downloader;
#endif /* USE_GRABBER */
  send[STEP_ENDING].handler      = VH_HANDLE->dbmanager;

  do
  {
    e = ACTION_NO_OPERATION;
    data = NULL;

    res = vh_fifo_queue_pop (dispatcher->fifo, &e, &data);
    if (res || e == ACTION_NO_OPERATION)
      continue;

    if (e == ACTION_KILL_THREAD)
      break;

    pdata = data;

    switch (e)
    {
    case ACTION_PAUSE_THREAD:
      VH_THREAD_PAUSE_ACTION (dispatcher)
      continue;

#ifdef USE_GRABBER
    case ACTION_DB_NEXT_LOOP:
      vh_grabber_action_send (VH_HANDLE->grabber,
                              FIFO_QUEUE_PRIORITY_NORMAL, e, NULL);
      break;
#endif /* USE_GRABBER */

    case ACTION_DB_INSERT_P:
    case ACTION_DB_INSERT_G:
    case ACTION_DB_UPDATE_P:
    case ACTION_DB_UPDATE_G:
    case ACTION_DB_END:
    {
      processing_step_t step = pdata->step;

      vh_log (VALHALLA_MSG_VERBOSE,
              "[%s] step: %i, file: \"%s\"",
              __FUNCTION__, step, pdata->file.path);

#ifdef USE_GRABBER
      /*
       * If step is GRABBING, then parsed data are added/updated for
       * the first grab, and grabbed data are added/updated for the
       * next potential grabbing. It depends if more than one grabber
       * is available.
       * If step is DOWNLOADING, then the last grabbed data is
       * added/updated.
       *
       * When skip is != 0, then pdata has no meta_grabber available
       * because all grabber threads are busy. In this case, nothing
       * must be sent to the DBManager.
       */
      if (step > STEP_PARSING && step < STEP_ENDING && !pdata->skip)
      {
        /*
         * Only one meta_grabber exists for all grabbers. It is necessary
         * to lock the metadata in the grabber until the semaphore is
         * released by the dbmanager, in order to prevent a race condition
         * between the insertion and the next grabber.
         */
        if (step == STEP_GRABBING
            && (e == ACTION_DB_INSERT_G || e == ACTION_DB_UPDATE_G))
          pdata->wait = 1;
#else /* USE_GRABBER */
      /* Parsed data added/updated. */
      if (step == STEP_ENDING)
      {
#endif /* !USE_GRABBER */
        vh_dbmanager_action_send (VH_HANDLE->dbmanager,
                                  pdata->priority, e, pdata);
      }

      if (step == STEP_ENDING)
      {
        e = ACTION_DB_END;
        /*
         * Force NORMAL priority because the last step must be always
         * at the end! It prevents to free pdata before the handling
         * of metadata.
         */
        pdata->priority = FIFO_QUEUE_PRIORITY_NORMAL;
      }

      /* Proceed to the step */
      send[step].fct (send[step].handler, pdata->priority, e, pdata);
      break;
    }

    default:
      break;
    }
  }
  while (!dispatcher_is_stopped (dispatcher));

  pthread_exit (NULL);
}

int
vh_dispatcher_run (dispatcher_t *dispatcher, int priority)
{
  int res = DISPATCHER_SUCCESS;
  pthread_attr_t attr;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!dispatcher)
    return DISPATCHER_ERROR_HANDLER;

  dispatcher->priority = priority;
  dispatcher->run      = 1;

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

  res =
    pthread_create (&dispatcher->thread, &attr, dispatcher_thread, dispatcher);
  if (res)
  {
    res = DISPATCHER_ERROR_THREAD;
    dispatcher->run = 0;
  }

  pthread_attr_destroy (&attr);
  return res;
}

fifo_queue_t *
vh_dispatcher_fifo_get (dispatcher_t *dispatcher)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!dispatcher)
    return NULL;

  return dispatcher->fifo;
}

void
vh_dispatcher_pause (dispatcher_t *dispatcher)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!dispatcher)
    return;

  VH_THREAD_PAUSE_FCT (dispatcher, 1)
}

void
vh_dispatcher_stop (dispatcher_t *dispatcher, int f)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!dispatcher)
    return;

  if (f & STOP_FLAG_REQUEST && !dispatcher_is_stopped (dispatcher))
  {
    pthread_mutex_lock (&dispatcher->mutex_run);
    dispatcher->run = 0;
    pthread_mutex_unlock (&dispatcher->mutex_run);

    vh_fifo_queue_push (dispatcher->fifo,
                        FIFO_QUEUE_PRIORITY_HIGH, ACTION_KILL_THREAD, NULL);
    dispatcher->wait = 1;

    VH_THREAD_PAUSE_FORCESTOP (dispatcher, 1)
  }

  if (f & STOP_FLAG_WAIT && dispatcher->wait)
  {
    pthread_join (dispatcher->thread, NULL);
    dispatcher->wait = 0;
  }
}

void
vh_dispatcher_uninit (dispatcher_t *dispatcher)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!dispatcher)
    return;

  vh_fifo_queue_free (dispatcher->fifo);
  pthread_mutex_destroy (&dispatcher->mutex_run);
  VH_THREAD_PAUSE_UNINIT (dispatcher)

  free (dispatcher);
}

dispatcher_t *
vh_dispatcher_init (valhalla_t *handle)
{
  dispatcher_t *dispatcher;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  dispatcher = calloc (1, sizeof (dispatcher_t));
  if (!dispatcher)
    return NULL;

  dispatcher->fifo = vh_fifo_queue_new ();
  if (!dispatcher->fifo)
    goto err;

  dispatcher->valhalla = handle; /* VH_HANDLE */

  pthread_mutex_init (&dispatcher->mutex_run, NULL);
  VH_THREAD_PAUSE_INIT (dispatcher)

  return dispatcher;

 err:
  vh_dispatcher_uninit (dispatcher);
  return NULL;
}

void
vh_dispatcher_action_send (dispatcher_t *dispatcher,
                           fifo_queue_prio_t prio, int action, void *data)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!dispatcher)
    return;

  vh_fifo_queue_push (dispatcher->fifo, prio, action, data);
}
