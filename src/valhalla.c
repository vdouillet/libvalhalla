/*
 * GeeXboX Valhalla: tiny media scanner API.
 * Copyright (C) 2009-2011 Mathieu Schroeter <mathieu@schroetersa.ch>
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
#include <stdlib.h>
#include <string.h>

#ifdef USE_LAVC
#include <libavcodec/avcodec.h>
#endif /* USE_LAVC */
#include <libavformat/avformat.h>

#include "valhalla.h"
#include "valhalla_internals.h"
#include "parser.h"
#include "scanner.h"
#include "dbmanager.h"
#include "dispatcher.h"
#include "ondemand.h"
#include "event_handler.h"
#include "utils.h"
#include "osdep.h"
#include "stats.h"
#include "metadata.h"
#include "logs.h"

#ifdef USE_GRABBER
#include "grabber.h"
#include "downloader.h"
#include "url_utils.h"
#endif /* USE_GRABBER */

static int g_preinit;
static pthread_mutex_t g_preinit_mutex = PTHREAD_MUTEX_INITIALIZER;


unsigned int
libvalhalla_version (void)
{
  return LIBVALHALLA_VERSION_INT;
}

/******************************************************************************/
/*                                                                            */
/*                             Valhalla Handling                              */
/*                                                                            */
/******************************************************************************/

static void
queue_cleanup (fifo_queue_t *queue)
{
  int e;
  void *data;

  vh_fifo_queue_push (queue,
                      FIFO_QUEUE_PRIORITY_NORMAL, ACTION_CLEANUP_END, NULL);

  do
  {
    e = ACTION_NO_OPERATION;
    data = NULL;
    vh_fifo_queue_pop (queue, &e, &data);

    switch (e)
    {
    default:
      break;

    case ACTION_DB_INSERT_P:
    case ACTION_DB_INSERT_G:
    case ACTION_DB_UPDATE_P:
    case ACTION_DB_UPDATE_G:
    case ACTION_DB_END:
    case ACTION_DB_NEWFILE:
      if (data)
        vh_file_data_free (data);
      break;

    case ACTION_DB_EXT_INSERT:
    case ACTION_DB_EXT_UPDATE:
    case ACTION_DB_EXT_DELETE:
    case ACTION_DB_EXT_PRIORITY:
      if (data)
        vh_dbmanager_extmd_free (data);
      break;

    case ACTION_OD_ENGAGE:
    case ACTION_EH_EVENTGL:
      if (data)
        free (data);
      break;

    case ACTION_EH_EVENTOD:
      if (data)
        vh_event_handler_od_free (data);
      break;

    case ACTION_EH_EVENTMD:
      if (data)
        vh_event_handler_md_free (data);
      break;
    }
  }
  while (e != ACTION_CLEANUP_END);
}

static void
valhalla_mrproper (valhalla_t *handle)
{
  unsigned int i;
  fifo_queue_t *fifo_o;

  fifo_queue_t *fifo_i[] = {
    vh_scanner_fifo_get (handle->scanner),
    vh_dbmanager_fifo_get (handle->dbmanager),
    vh_dispatcher_fifo_get (handle->dispatcher),
    vh_parser_fifo_get (handle->parser),
#ifdef USE_GRABBER
    vh_grabber_fifo_get (handle->grabber),
    vh_downloader_fifo_get (handle->downloader),
#endif /* USE_GRABBER */
    vh_event_handler_fifo_get (handle->event_handler),
  };

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return;

  fifo_o = vh_fifo_queue_new ();
  if (!fifo_o)
    return;

#ifdef USE_GRABBER
  vh_dbmanager_db_begin_transaction (handle->dbmanager);

  /* remove all previous contexts */
  vh_dbmanager_db_dlcontext_delete (handle->dbmanager);
#endif /* USE_GRABBER */

  /*
   * The same data pointer can exist in several queues at the same time.
   * The goal is to identify all unique entries from all queues and to save
   * these entries in the fifo_o queue.
   * Then, all data pointers can be safety freed (prevents double free).
   */
  for (i = 0; i < ARRAY_NB_ELEMENTS (fifo_i); i++)
  {
    int e;
    void *data;

    if (!fifo_i[i])
      continue;

    vh_fifo_queue_push (fifo_i[i],
                        FIFO_QUEUE_PRIORITY_NORMAL, ACTION_CLEANUP_END, NULL);

    do
    {
      e = ACTION_NO_OPERATION;
      data = NULL;
      vh_fifo_queue_pop (fifo_i[i], &e, &data);

      switch (e)
      {
      case ACTION_DB_INSERT_P:
      case ACTION_DB_INSERT_G:
      case ACTION_DB_UPDATE_P:
      case ACTION_DB_UPDATE_G:
      case ACTION_DB_END:
      case ACTION_DB_NEWFILE:
      {
        file_data_t *file = data;
        if (!file || file->clean_f)
          break;

        file->clean_f = 1;
        vh_fifo_queue_push (fifo_o, FIFO_QUEUE_PRIORITY_NORMAL, e, data);

#ifdef USE_GRABBER
        /* save downloader context */
        if (file->step < STEP_ENDING && file->list_downloader)
          vh_dbmanager_db_dlcontext_save (handle->dbmanager, file);
#endif /* USE_GRABBER */
        break;
      }

      case ACTION_DB_EXT_INSERT:
      case ACTION_DB_EXT_UPDATE:
      case ACTION_DB_EXT_DELETE:
      case ACTION_EH_EVENTOD:
      case ACTION_EH_EVENTMD:
      case ACTION_EH_EVENTGL:
        vh_fifo_queue_push (fifo_o, FIFO_QUEUE_PRIORITY_NORMAL, e, data);
        break;

      default:
        break;
      }
    }
    while (e != ACTION_CLEANUP_END);
  }

#ifdef USE_GRABBER
  vh_dbmanager_db_end_transaction (handle->dbmanager);
#endif /* USE_GRABBER */

  queue_cleanup (fifo_o);
  vh_fifo_queue_free (fifo_o);

  /* On-demand queue must be handled separately. */
  fifo_o = vh_ondemand_fifo_get (handle->ondemand);
  if (!fifo_o)
    return;
  queue_cleanup (fifo_o);
}

int
valhalla_config_set_orig (valhalla_t *handle, valhalla_cfg_t conf, ...)
{
  int res = 0;
  const void *p1 = NULL, *p2 = NULL;
  int i = 0;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return -1;

  if (conf >= (1 << VH_CFG_RANGE))
  {
    va_list ap;

    va_start (ap, conf);

    if (conf & VH_VOIDP_T)
      p1 = va_arg (ap, void *);
    if (conf & VH_INT_T)
      i  = va_arg (ap, int);
    if (conf & VH_VOIDP_2_T)
      p2 = va_arg (ap, void *);

    if (va_arg (ap, int) != ~0) /* check for safeguard */
    {
      vh_log (VALHALLA_MSG_CRITICAL,
              "unrecoverable error with valhalla_config_set(), conf = %#x, "
              "it is probably a bad use of this function", conf);
      res = -2;
    }

    va_end (ap);
  }

  if (res)
    return res;

  switch (conf)
  {
#ifdef USE_GRABBER
  case VALHALLA_CFG_DOWNLOADER_DEST:
    vh_downloader_destination_set (handle->downloader, (valhalla_dl_t) i, p1);
    break;

  case VALHALLA_CFG_GRABBER_PRIORITY:
    vh_grabber_priority_set (handle->grabber,
                             p1, (valhalla_metadata_pl_t) i, p2);
    break;

  case VALHALLA_CFG_GRABBER_STATE:
    if (p1)
      vh_grabber_state_set (handle->grabber, p1, i);
    break;
#endif /* USE_GRABBER */

  case VALHALLA_CFG_PARSER_KEYWORD:
    if (p1)
      vh_parser_bl_keyword_add (handle->parser, p1);
    break;

  case VALHALLA_CFG_SCANNER_PATH:
    if (p1)
      vh_scanner_path_add (handle->scanner, p1, i);
    break;

  case VALHALLA_CFG_SCANNER_SUFFIX:
    if (p1)
      vh_scanner_suffix_add (handle->scanner, p1);
    break;

  default:
    vh_log (VALHALLA_MSG_WARNING,
            "%s: unsupported option %#x", __FUNCTION__, conf);
    res = -1;
    break;
  }

  return res;
}

void
valhalla_wait (valhalla_t *handle)
{
  const int f = STOP_FLAG_REQUEST | STOP_FLAG_WAIT;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || handle->noscan)
    return;

  vh_scanner_wait (handle->scanner);

  vh_ondemand_stop (handle->ondemand, f);
  vh_dbmanager_wait (handle->dbmanager);
  vh_dispatcher_stop (handle->dispatcher, f);
  vh_parser_stop (handle->parser, f);
#ifdef USE_GRABBER
  vh_grabber_stop (handle->grabber, f);
  vh_downloader_stop (handle->downloader, f);
#endif /* USE_GRABBER */
  vh_event_handler_stop (handle->event_handler, f);

  handle->fstop = 1;
}

static void
valhalla_force_stop (valhalla_t *handle)
{
  unsigned int i;

  vh_log (VALHALLA_MSG_WARNING,
          "%s: This can take a time, please be patient", __FUNCTION__);

  for (i = 0; i < 2; i++)
  {
    int f = !i ? STOP_FLAG_REQUEST : STOP_FLAG_WAIT;

    vh_ondemand_stop (handle->ondemand, f);
    if (!handle->noscan)
      vh_scanner_stop (handle->scanner, f);
    vh_dbmanager_stop (handle->dbmanager, f);
    vh_dispatcher_stop (handle->dispatcher, f);
    vh_parser_stop (handle->parser, f);
#ifdef USE_GRABBER
    vh_grabber_stop (handle->grabber, f);
    vh_downloader_stop (handle->downloader, f);
#endif /* USE_GRABBER */
    vh_event_handler_stop (handle->event_handler, f);

#ifdef USE_GRABBER
    /* abort all cURL transfers as fast as possible */
    if (f == STOP_FLAG_REQUEST)
      vh_url_ctl_abort (handle->url_ctl);
#endif /* USE_GRABBER */
  }

  valhalla_mrproper (handle);

  handle->fstop = 1;
}

void
valhalla_uninit (valhalla_t *handle)
{
  int preinit;

  vh_log (VALHALLA_MSG_VERBOSE, "%s: begin", __FUNCTION__);

  if (!handle)
    return;

  pthread_mutex_lock (&g_preinit_mutex);
  preinit = --g_preinit;
  pthread_mutex_unlock (&g_preinit_mutex);

  if (!handle->fstop)
    valhalla_force_stop (handle);

  /* dump all statistics */
  vh_stats_dump (handle->stats, NULL);
  vh_stats_debug_dump (handle->stats);

  vh_ondemand_uninit (handle->ondemand);
  vh_scanner_uninit (handle->scanner);
  vh_dbmanager_uninit (handle->dbmanager);
  vh_dispatcher_uninit (handle->dispatcher);
  vh_parser_uninit (handle->parser);
#ifdef USE_GRABBER
  vh_grabber_uninit (handle->grabber);
  vh_downloader_uninit (handle->downloader);
#endif /* USE_GRABBER */
  vh_event_handler_uninit (handle->event_handler);

#if USE_GRABBER
  vh_url_global_uninit ();
  vh_url_ctl_free (handle->url_ctl);
#endif /* USE_GRABBER */

#ifdef USE_LAVC
  if (!preinit)
    av_lockmgr_register (NULL);
#endif /* USE_LAVC */

  vh_stats_free (handle->stats);

  vh_log (VALHALLA_MSG_VERBOSE, "%s: end", __FUNCTION__);

  free (handle);
}

int
valhalla_run (valhalla_t *handle,
              int loop, uint16_t timeout, uint16_t delay, int priority)
{
  int res;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return VALHALLA_ERROR_HANDLER;

  if (handle->run)
    return VALHALLA_ERROR_DEAD;

  handle->run = 1;

  if (handle->event_handler)
  {
    res = vh_event_handler_run (handle->event_handler, priority);
    if (res)
      return VALHALLA_ERROR_THREAD;
  }

  res = vh_scanner_run (handle->scanner, loop, timeout, delay, priority);
  if (res)
  {
    if (res == SCANNER_ERROR_PATH)
    {
      handle->noscan = 1;
      vh_log (VALHALLA_MSG_INFO , "no path defined, scanner disabled");
    }
    else
      return VALHALLA_ERROR_THREAD;
  }

  res = vh_dbmanager_run (handle->dbmanager, priority);
  if (res)
    return VALHALLA_ERROR_THREAD;

  res = vh_dispatcher_run (handle->dispatcher, priority);
  if (res)
    return VALHALLA_ERROR_THREAD;

  res = vh_parser_run (handle->parser, priority);
  if (res)
    return VALHALLA_ERROR_THREAD;

#ifdef USE_GRABBER
  res = vh_grabber_run (handle->grabber, priority);
  if (res)
    return VALHALLA_ERROR_THREAD;

  res = vh_downloader_run (handle->downloader, priority);
  if (res)
    return VALHALLA_ERROR_THREAD;
#endif /* USE_GRABBER */

  res = vh_ondemand_run (handle->ondemand, priority);
  if (res)
    return VALHALLA_ERROR_THREAD;

  return VALHALLA_SUCCESS;
}

const char *
valhalla_metadata_group_str (valhalla_meta_grp_t group)
{
  return vh_metadata_group_str (group);
}

const char *
valhalla_grabber_next (valhalla_t *handle, const char *id)
{
  vh_log (VALHALLA_MSG_VERBOSE, "%s : %s", __FUNCTION__, id ? id : "");

  if (!handle)
    return NULL;

#ifdef USE_GRABBER
  return vh_grabber_next (handle->grabber, id);
#else /* USE_GRABBER */
  vh_log (VALHALLA_MSG_WARNING,
          "This function is usable only with grabbing support!");
  return NULL;
#endif /* !USE_GRABBER */
}

valhalla_metadata_pl_t
valhalla_grabber_priority_read (valhalla_t *handle,
                                const char *id, const char **meta)
{
  vh_log (VALHALLA_MSG_VERBOSE,
          "%s : %s/%s", __FUNCTION__, id ? id : "", meta && *meta ? *meta : "");

  if (!handle)
    return 0;

#ifdef USE_GRABBER
  return vh_grabber_priority_read (handle->grabber, id, meta);
#else /* USE_GRABBER */
  vh_log (VALHALLA_MSG_WARNING,
          "This function is usable only with grabbing support!");
  return 0;
#endif /* !USE_GRABBER */
}

const char *
valhalla_stats_group_next (valhalla_t *handle, const char *id)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  return vh_stats_group_next (handle->stats, id);
}

uint64_t
valhalla_stats_read_next (valhalla_t *handle, const char *id,
                          valhalla_stats_type_t type, const char **item)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return 0;

  return vh_stats_read_next (handle->stats, id, type, item);
}

void
valhalla_verbosity (valhalla_verb_t level)
{
  vh_log_verb (level);
}

#ifdef USE_LAVC
static int
valhalla_avlock (void **mutex, enum AVLockOp op)
{
  switch (op)
  {
  case AV_LOCK_CREATE:
    *mutex = malloc (sizeof (pthread_mutex_t));
    if (!*mutex)
      return 1;
    return !!pthread_mutex_init (*mutex, NULL);

  case AV_LOCK_OBTAIN:
    return !!pthread_mutex_lock (*mutex);

  case AV_LOCK_RELEASE:
    return !!pthread_mutex_unlock (*mutex);

  case AV_LOCK_DESTROY:
    pthread_mutex_destroy (*mutex);
    free (*mutex);
    return 0;

  default:
    return 1;
  }
}
#endif /* USE_LAVC */

valhalla_t *
valhalla_init (const char *db, valhalla_init_param_t *param)
{
  int preinit;
  valhalla_t *handle;
  valhalla_init_param_t p;
  const valhalla_init_param_t *pp = &p;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  pthread_mutex_lock (&g_preinit_mutex);
  preinit = g_preinit;
  pthread_mutex_unlock (&g_preinit_mutex);

  if (!preinit && vh_osdep_init ())
    return NULL;

  if (!db)
    return NULL;

  if (param)
    pp = param;
  else
    memset (&p, 0, sizeof (p));

  handle = calloc (1, sizeof (valhalla_t));
  if (!handle)
    return NULL;

#ifdef USE_GRABBER
  handle->url_ctl = vh_url_ctl_new ();
  if (!handle->url_ctl)
    return NULL;

  vh_url_global_init ();
#endif /* USE_GRABBER */

  handle->stats = vh_stats_new ();
  if (!handle->stats)
    goto err;

  if (pp->od_cb || pp->gl_cb || pp->md_cb)
  {
    event_handler_cb_t cb;

    cb.od_cb = pp->od_cb;
    cb.gl_cb = pp->gl_cb;
    cb.md_cb = pp->md_cb;
    cb.od_data = pp->od_data;
    cb.gl_data = pp->gl_data;
    cb.md_data = pp->md_data;

    handle->event_handler = vh_event_handler_init (handle, &cb, pp->od_meta);
    if (!handle->event_handler)
      goto err;
  }

  handle->dispatcher = vh_dispatcher_init (handle);
  if (!handle->dispatcher)
    goto err;

  handle->parser = vh_parser_init (handle, pp->parser_nb, pp->decrapifier);
  if (!handle->parser)
    goto err;

#ifdef USE_GRABBER
  handle->grabber = vh_grabber_init (handle, pp->grabber_nb);
  if (!handle->grabber)
    goto err;

  handle->downloader = vh_downloader_init (handle);
  if (!handle->downloader)
    goto err;
#endif /* USE_GRABBER */

  handle->scanner = vh_scanner_init (handle);
  if (!handle->scanner)
    goto err;

  handle->dbmanager = vh_dbmanager_init (handle, db, pp->commit_int);
  if (!handle->dbmanager)
    goto err;

  handle->ondemand = vh_ondemand_init (handle);
  if (!handle->ondemand)
    goto err;

  if (!preinit)
  {
#ifdef USE_LAVC
    if (av_lockmgr_register (valhalla_avlock))
      goto err;
#endif /* USE_LAVC */
    av_log_set_level (AV_LOG_FATAL);
    av_register_all ();
  }

  pthread_mutex_lock (&g_preinit_mutex);
  g_preinit++;
  pthread_mutex_unlock (&g_preinit_mutex);

  return handle;

 err:
  valhalla_uninit (handle);
  return NULL;
}

void
valhalla_scanner_wakeup (valhalla_t *handle)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return;

  vh_scanner_wakeup (handle->scanner);
}

void
valhalla_ondemand (valhalla_t *handle, const char *file)
{
  char *odfile;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !file)
    return;

  odfile = strdup (file);
  if (!odfile)
    return;

  vh_ondemand_action_send (handle->ondemand, FIFO_QUEUE_PRIORITY_HIGH,
                           ACTION_OD_ENGAGE, odfile);
}

const char *
valhalla_ondemand_cb_meta (valhalla_t *handle, const char *meta)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  return vh_event_handler_od_cb_meta (handle->event_handler, meta);
}

/******************************************************************************/
/*                                                                            */
/*                         Public Database Selections                         */
/*                                                                            */
/******************************************************************************/

valhalla_db_stmt_t *
valhalla_db_metalist_get (valhalla_t *handle, valhalla_db_item_t *search,
                          valhalla_file_type_t filetype,
                          valhalla_db_restrict_t *restriction)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !search)
    return NULL;

  return vh_dbmanager_db_metalist_get (handle->dbmanager,
                                       search, filetype, restriction);
}

const valhalla_db_metares_t *
valhalla_db_metalist_read (valhalla_t *handle, valhalla_db_stmt_t *vhstmt)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !vhstmt)
    return NULL;

  return vh_dbmanager_db_metalist_read (handle->dbmanager, vhstmt);
}

valhalla_db_stmt_t *
valhalla_db_filelist_get (valhalla_t *handle, valhalla_file_type_t filetype,
                          valhalla_db_restrict_t *restriction)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  return
    vh_dbmanager_db_filelist_get (handle->dbmanager, filetype, restriction);
}

const valhalla_db_fileres_t *
valhalla_db_filelist_read (valhalla_t *handle, valhalla_db_stmt_t *vhstmt)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !vhstmt)
    return NULL;

  return vh_dbmanager_db_filelist_read (handle->dbmanager, vhstmt);
}

valhalla_db_stmt_t *
valhalla_db_file_get (valhalla_t *handle, int64_t id, const char *path,
                      valhalla_db_restrict_t *restriction)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  return vh_dbmanager_db_file_get (handle->dbmanager, id, path, restriction);
}

const valhalla_db_metares_t *
valhalla_db_file_read (valhalla_t *handle, valhalla_db_stmt_t *vhstmt)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !vhstmt)
    return NULL;

  return vh_dbmanager_db_file_read (handle->dbmanager, vhstmt);
}

/******************************************************************************/
/*                                                                            */
/*                  For Public Insertions/Updates/Deletions                   */
/*                                                                            */
/******************************************************************************/

int
valhalla_db_metadata_insert (valhalla_t *handle, const char *path,
                             const char *meta, const char *data,
                             valhalla_lang_t lang, valhalla_meta_grp_t group)
{
  dbmanager_extmd_t *extmd;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !path || !meta || !data)
    return -1;

  extmd = calloc (1, sizeof (dbmanager_extmd_t));
  if (!extmd)
    return -1;

  extmd->path  = strdup (path);
  extmd->meta  = strdup (meta);
  extmd->data  = strdup (data);
  extmd->group = group;
  extmd->lang  = lang;

  vh_dbmanager_action_send (handle->dbmanager, FIFO_QUEUE_PRIORITY_HIGH,
                            ACTION_DB_EXT_INSERT, extmd);
  return 0;
}

int
valhalla_db_metadata_update (valhalla_t *handle, const char *path,
                             const char *meta, const char *data,
                             const char *ndata, valhalla_lang_t lang)
{
  dbmanager_extmd_t *extmd;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !path || !meta || !data || !ndata)
    return -1;

  extmd = calloc (1, sizeof (dbmanager_extmd_t));
  if (!extmd)
    return -1;

  extmd->path  = strdup (path);
  extmd->meta  = strdup (meta);
  extmd->data  = strdup (data);
  extmd->ndata = strdup (ndata);
  extmd->lang  = lang;

  vh_dbmanager_action_send (handle->dbmanager, FIFO_QUEUE_PRIORITY_HIGH,
                            ACTION_DB_EXT_UPDATE, extmd);
  return 0;
}

int
valhalla_db_metadata_delete (valhalla_t *handle, const char *path,
                             const char *meta, const char *data)
{
  dbmanager_extmd_t *extmd;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !path || !meta || !data)
    return -1;

  extmd = calloc (1, sizeof (dbmanager_extmd_t));
  if (!extmd)
    return -1;

  extmd->path  = strdup (path);
  extmd->meta  = strdup (meta);
  extmd->data  = strdup (data);

  vh_dbmanager_action_send (handle->dbmanager, FIFO_QUEUE_PRIORITY_HIGH,
                            ACTION_DB_EXT_DELETE, extmd);
  return 0;
}

int
valhalla_db_metadata_priority (valhalla_t *handle, const char *path,
                               const char *meta, const char *data,
                               valhalla_metadata_pl_t p)
{
  dbmanager_extmd_t *extmd;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle || !path)
    return -1;

  if (!meta && data)
    return -1;

  extmd = calloc (1, sizeof (dbmanager_extmd_t));
  if (!extmd)
    return -1;

  extmd->path = strdup (path);
  if (meta)
    extmd->meta = strdup (meta);
  if (data)
    extmd->data = strdup (data);
  extmd->priority = p;

  vh_dbmanager_action_send (handle->dbmanager, FIFO_QUEUE_PRIORITY_HIGH,
                            ACTION_DB_EXT_PRIORITY, extmd);
  return 0;
}
