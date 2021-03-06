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

#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>

#include "valhalla.h"
#include "valhalla_internals.h"
#include "utils.h"
#include "osdep.h"
#include "fifo_queue.h"
#include "logs.h"
#include "lavf_utils.h"
#include "metadata.h"
#include "thread_utils.h"
#include "dbmanager.h"
#include "parser.h"
#include "dispatcher.h"

#ifndef PARSER_NB_MAX
#define PARSER_NB_MAX 8
#endif /* PARSER_NB_MAX */

#define VH_HANDLE parser->valhalla

#define IS_TO_DECRAPIFY(c)                \
 ((unsigned) (c) <= 0x7F                  \
  && (c) != '\''                          \
  && !VH_ISSPACE (c)                      \
  && !VH_ISALNUM (c))

struct parser_s {
  valhalla_t   *valhalla;
  pthread_t     thread[PARSER_NB_MAX];
  fifo_queue_t *fifo;
  unsigned int  nb;
  int           priority;

  int    decrapifier;
  char **bl_list;

  int             wait;
  int             run;
  pthread_mutex_t mutex_run;

  VH_THREAD_PAUSE_ATTRS
};


static inline int
parser_is_stopped (parser_t *parser)
{
  int run;
  pthread_mutex_lock (&parser->mutex_run);
  run = parser->run;
  pthread_mutex_unlock (&parser->mutex_run);
  return !run;
}

/* VH_TEST (parser_decrap_pattern) { */
#define PATTERN_NUMBER "NUM"
#define PATTERN_SEASON  "SE"
#define PATTERN_EPISODE "EP"

static void
parser_decrap_pattern (char *str, const char *bl,
                       unsigned int *se, unsigned int *ep)
{
  char pattern[64];
  int res, len_s = 0, len_e = 0;
  char *it, *it1, *it2;

  /* prepare pattern */
  snprintf (pattern, sizeof (pattern), "%%n%s%%n", bl);
  while ((it = strstr (pattern, PATTERN_NUMBER)))
    memcpy (it, "%*u", 3);

  it1 = strstr (pattern, PATTERN_SEASON);
  it2 = strstr (pattern, PATTERN_EPISODE);
  if (it1)
    memcpy (it1, "%u", 2);
  if (it2)
    memcpy (it2, "%u", 2);

  /* search pattern in the string */
  it = str;
  do
  {
    len_s = 0;
    if (!it1 && it2) /* only EP */
      res = sscanf (it, pattern, &len_s, ep, &len_e);
    else if (!it2 && it1) /* only SE */
      res = sscanf (it, pattern, &len_s, se, &len_e);
    else if (it1 && it2) /* SE & EP */
    {
      if (it1 < it2)
        res = sscanf (it, pattern, &len_s, se, ep, &len_e);
      else
        res = sscanf (it, pattern, &len_s, ep, se, &len_e);
      /* Both must return something with sscanf. */
      if (!*ep)
        *se = 0;
      if (!*se)
        *ep = 0;
    }
    else /* NUM */
      res = sscanf (it, pattern, &len_s, &len_e);
    it += len_s + 1;
  }
  while (!len_e && res != EOF);

  if (len_e <= len_s)
    return;

  it--;

  /*
   * Check if the pattern found is not into a string. Space, start or end
   * of string must be found before and after the part corresponding to
   * the pattern.
   */
  if ((it == str || *it == ' ' || *(it - 1) == ' ') /* nothing or ' ' before */
      && (   *(it + len_e - len_s) == ' '           /* ' ' or nothing after  */
          || *(it + len_e - len_s) == '\0'))
    /* cleanup */
    memset (it, ' ', len_e - len_s);
  else
  {
    *se = 0;
    *ep = 0;
  }
}
/* } VH_TEST (parser_decrap_pattern) */

static void
parser_decrap_blacklist (char **list, char *str, metadata_t **meta)
{
  char **l;

  for (l = list; l && *l; l++)
  {
    size_t size;
    char *p;

    if (   strstr (*l, PATTERN_NUMBER)
        || strstr (*l, PATTERN_SEASON)
        || strstr (*l, PATTERN_EPISODE))
    {
      unsigned int i, se = 0, ep = 0;

      parser_decrap_pattern (str, *l, &se, &ep);
      for (i = 0; i < 2; i++)
      {
        unsigned int val = i ? ep : se;
        char v[32];

        if (!val)
          continue;

        snprintf (v, sizeof (v), "%u", val);
        if (i)
          vh_metadata_add_auto (meta, VALHALLA_METADATA_EPISODE,
                                v, VALHALLA_LANG_UNDEF, NULL);
        else
          vh_metadata_add_auto (meta, VALHALLA_METADATA_SEASON,
                                v, VALHALLA_LANG_UNDEF, NULL);
      }
      continue;
    }

    p = strcasestr (str, *l);
    if (!p)
      continue;

    size = strlen (*l);
    if (!VH_ISGRAPH (*(p + size)) && (p == str || !VH_ISGRAPH (*(p - 1))))
      memset (p, ' ', size);
  }
}

static void
parser_decrap_cleanup (char *str)
{
  char *clean, *clean_it;
  char *it;

  clean = malloc (strlen (str) + 1);
  if (!clean)
    return;
  clean_it = clean;

  for (it = str; *it; it++)
  {
    if (!VH_ISSPACE (*it))
    {
      *clean_it = *it;
      clean_it++;
    }
    else if (!VH_ISSPACE (*(it + 1)) && *(it + 1) != '\0')
    {
      *clean_it = ' ';
      clean_it++;
    }
  }

  /* remove spaces after */
  while (clean_it > str && VH_ISSPACE (*(clean_it - 1)))
    clean_it--;
  *clean_it = '\0';

  /* remove spaces before */
  clean_it = clean;
  while (VH_ISSPACE (*clean_it))
    clean_it++;

  strcpy (str, clean_it);
  free (clean);
}

static char *
parser_decrapify (parser_t *parser, const char *file, metadata_t **meta)
{
  char *it, *filename, *res;
  char *file_tmp = strdup (file);

  if (!file_tmp)
    return NULL;

  it = strrchr (file_tmp, '.');
  if (it) /* trim suffix */
    *it = '\0';
  it = strrchr (file_tmp, '/');
  if (it) /* trim path */
    it++;
  else
  {
    free (file_tmp);
    return NULL;
  }

  filename = it;

  /* decrapify */
  for (; *it; it++)
    if (IS_TO_DECRAPIFY (*it))
      *it = ' ';

  parser_decrap_blacklist (parser->bl_list, filename, meta);
  parser_decrap_cleanup (filename);

  res = strdup (filename);
  free (file_tmp);

  vh_log (VALHALLA_MSG_VERBOSE, "decrapifier: \"%s\"", res);

  return res;
}

static metadata_t *
parser_metadata_get (parser_t *parser, AVFormatContext *ctx, const char *file)
{
  unsigned int i;
  metadata_t *meta = NULL;
  const metadata_t *title_tag = NULL;
  AVDictionaryEntry *tag = NULL;
  const metadata_plist_t pl = {
    .metadata = NULL,
    .priority = VALHALLA_METADATA_PL_HIGHEST
  };

  if (!ctx)
    return NULL;

  while ((tag = av_dict_get (ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    vh_metadata_add_auto (&meta, tag->key, tag->value,
                          VALHALLA_LANG_UNDEF, &pl);

  for (i = 0; i < ctx->nb_streams; i++)
  {
    AVStream *st = ctx->streams[i];
    while ((tag = av_dict_get (st->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    {
      const char *key = tag->key;

      /* without this check, it is possible to have more than one title */
      if (vh_metadata_get (meta, VALHALLA_METADATA_TITLE, 0, &title_tag)
          && !strcasecmp (key, VALHALLA_METADATA_TITLE))
        key = VALHALLA_METADATA_TITLE_STREAM;

      vh_metadata_add_auto (&meta, key, tag->value,
                            VALHALLA_LANG_UNDEF, &pl);
    }
  }

  if (!meta)
    vh_log (VALHALLA_MSG_VERBOSE, "no available metadata for %s", file);

  /* if necessary, use the filename as title */
  if (parser->decrapifier
      && vh_metadata_get (meta, VALHALLA_METADATA_TITLE, 0, &title_tag))
  {
    char *title = parser_decrapify (parser, file, &meta);
    if (title)
    {
      vh_metadata_add (&meta, VALHALLA_METADATA_TITLE, title,
                       VALHALLA_LANG_UNDEF, VALHALLA_META_GRP_TITLES,
                       VALHALLA_METADATA_PL_NORMAL);
      free (title);
    }
  }

  return meta;
}

static valhalla_file_type_t
parser_stream_info (AVFormatContext *ctx)
{
  int video_st = 0, audio_st = 0;
  unsigned int i;

  if (!ctx)
    return VALHALLA_FILE_TYPE_NULL;

  for (i = 0; i < ctx->nb_streams; i++)
  {
    AVStream *st = ctx->streams[i];
    if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
      video_st = 1;
    else if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO)
      audio_st = 1;
  }

  if (video_st)
  {
    if (!audio_st && ctx->nb_streams == 1
        && !strcmp (ctx->iformat->name, "image2"))
      return VALHALLA_FILE_TYPE_IMAGE;

    return VALHALLA_FILE_TYPE_VIDEO;
  }

  if (audio_st)
    return VALHALLA_FILE_TYPE_AUDIO;

  return VALHALLA_FILE_TYPE_NULL;
}

static void
parser_metadata (parser_t *parser, file_data_t *data)
{
  AVFormatContext *ctx;

  ctx = vh_lavf_utils_open_input_file (data->file.path);
  if (!ctx)
    return;

  data->file.type = parser_stream_info (ctx);
  data->meta_parser = parser_metadata_get (parser, ctx, data->file.path);

  // FIXME Some versions of ffmpeg might fail if name is null
  ctx->iformat->name = malloc(sizeof(char));
  if(ctx->iformat->name)
	  avformat_close_input (&ctx);
}

static void *
parser_thread (void *arg)
{
  int res, tid;
  int e;
  void *data = NULL;
  file_data_t *pdata;
  parser_t *parser = arg;

  if (!parser)
    pthread_exit (NULL);

  tid = vh_setpriority (parser->priority);

  vh_log (VALHALLA_MSG_VERBOSE,
          "[%s] tid: %i priority: %i", __FUNCTION__, tid, parser->priority);

  do
  {
    e = ACTION_NO_OPERATION;
    data = NULL;

    res = vh_fifo_queue_pop (parser->fifo, &e, &data);
    if (res || e == ACTION_NO_OPERATION)
      continue;

    if (e == ACTION_KILL_THREAD)
      break;

    if (e == ACTION_PAUSE_THREAD)
    {
      VH_THREAD_PAUSE_ACTION (parser)
      continue;
    }

    pdata = data;
    if (pdata)
      parser_metadata (parser, pdata);

    vh_file_data_step_increase (pdata, &e);
    vh_dispatcher_action_send (VH_HANDLE->dispatcher,
                               pdata->priority, e, pdata);
  }
  while (!parser_is_stopped (parser));

  pthread_exit (NULL);
}

int
vh_parser_run (parser_t *parser, int priority)
{
  int res = PARSER_SUCCESS;
  unsigned int i;
  pthread_attr_t attr;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return PARSER_ERROR_HANDLER;

  parser->priority = priority;
  parser->run      = 1;

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

  for (i = 0; i < parser->nb; i++)
  {
    res = pthread_create (&parser->thread[i], &attr, parser_thread, parser);
    if (res)
    {
      res = PARSER_ERROR_THREAD;
      parser->run = 0;
      break;
    }
  }

  pthread_attr_destroy (&attr);
  return res;
}

void
vh_parser_bl_keyword_add (parser_t *parser, const char *keyword)
{
  int n;
  char *const *it;
  const char *it2;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser || !parser->decrapifier)
    return;

  for (it2 = keyword; *it2; it2++)
    if (IS_TO_DECRAPIFY (*it2))
      return;

  /* check if the keyword is already in the list */
  for (it = parser->bl_list; it && *it; it++)
    if (!strcmp (*it, keyword))
      return;

  n = vh_get_list_length (parser->bl_list) + 1;
  parser->bl_list =
    realloc (parser->bl_list, (n + 1) * sizeof (*parser->bl_list));
  if (!parser->bl_list)
    return;

  parser->bl_list[n] = NULL;
  parser->bl_list[n - 1] = strdup (keyword);
}

fifo_queue_t *
vh_parser_fifo_get (parser_t *parser)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return NULL;

  return parser->fifo;
}

void
vh_parser_pause (parser_t *parser)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  VH_THREAD_PAUSE_FCT (parser, parser->nb)
}

void
vh_parser_stop (parser_t *parser, int f)
{
  unsigned int i;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  if (f & STOP_FLAG_REQUEST && !parser_is_stopped (parser))
  {
    pthread_mutex_lock (&parser->mutex_run);
    parser->run = 0;
    pthread_mutex_unlock (&parser->mutex_run);

    for (i = 0; i < parser->nb; i++)
      vh_fifo_queue_push (parser->fifo,
                          FIFO_QUEUE_PRIORITY_HIGH, ACTION_KILL_THREAD, NULL);
    parser->wait = 1;

    VH_THREAD_PAUSE_FORCESTOP (parser, parser->nb)
  }

  if (f & STOP_FLAG_WAIT && parser->wait)
  {
    for (i = 0; i < parser->nb; i++)
      pthread_join (parser->thread[i], NULL);
    parser->wait = 0;
  }
}

void
vh_parser_uninit (parser_t *parser)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  if (parser->bl_list)
  {
    char **bl;
    for (bl = parser->bl_list; *bl; bl++)
      free (*bl);
    free (parser->bl_list);
  }

  vh_fifo_queue_free (parser->fifo);
  pthread_mutex_destroy (&parser->mutex_run);
  VH_THREAD_PAUSE_UNINIT (parser)

  free (parser);
}

parser_t *
vh_parser_init (valhalla_t *handle, unsigned int nb, unsigned int decrapifier)
{
  parser_t *parser;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  parser = calloc (1, sizeof (parser_t));
  if (!parser)
    return NULL;

  if (nb > ARRAY_NB_ELEMENTS (parser->thread))
    goto err;

  parser->fifo = vh_fifo_queue_new ();
  if (!parser->fifo)
    goto err;

  parser->valhalla    = handle; /* VH_HANDLE */
  parser->nb          = nb ? nb : PARSER_NUMBER_DEF;
  parser->decrapifier = !!decrapifier;

  pthread_mutex_init (&parser->mutex_run, NULL);
  VH_THREAD_PAUSE_INIT (parser)

  return parser;

 err:
  vh_parser_uninit (parser);
  return NULL;
}

void
vh_parser_action_send (parser_t *parser,
                       fifo_queue_prio_t prio, int action, void *data)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  vh_fifo_queue_push (parser->fifo, prio, action, data);
}
