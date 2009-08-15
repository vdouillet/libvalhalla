/*
 * GeeXboX Valhalla: tiny media scanner API.
 * Copyright (C) 2009 Mathieu Schroeter <mathieu.schroeter@gamesover.ch>
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
 * Foundation, Inc, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>

#include "valhalla.h"
#include "valhalla_internals.h"
#include "utils.h"
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

struct parser_s {
  valhalla_t   *valhalla;
  pthread_t     thread[PARSER_NB_MAX];
  fifo_queue_t *fifo;
  unsigned int  nb;
  int           priority;

  int    decrapifier;
  char **bl_list;

  int             run;
  pthread_mutex_t mutex_run;
};


static const char *
suffix_fmt_guess (const char *file)
{
  const char *it;

  if (!file)
    return NULL;

  it = strrchr (file, '.');
  if (it)
    it++;

  return lavf_utils_fmtname_get (it);
}

static inline int
parser_is_stopped (parser_t *parser)
{
  int run;
  pthread_mutex_lock (&parser->mutex_run);
  run = parser->run;
  pthread_mutex_unlock (&parser->mutex_run);
  return !run;
}

static void
parser_metadata_group (metadata_t **meta,
                       const char *fmtname, const char *key, const char *value)
{
  /*
   * This array provides a list of keys for the attribution of group. The name
   * of the fmt is used for special cases where a key must be used with a group
   * different of the default attribution (default attributions are identified
   * by fmtname to NULL).
   * If the key can't be identified in the list, the default group is set.
   *
   * FIXME: the list must be completed.
   *        http://age.hobba.nl/audio/tag_frame_reference.html
   */
  static const struct metagrp_s {
    const char *key;
    const char *fmtname; /* special case for a specific AVInputFormat */
    const valhalla_meta_grp_t grp;
  } metagrp[] = {
    /* special attributions */
    /* ... */

    /* default attributions */
    { "album",      NULL,         VALHALLA_META_GRP_TITLES         },
    { "artist",     NULL,         VALHALLA_META_GRP_ENTITIES       },
    { "author",     NULL,         VALHALLA_META_GRP_ENTITIES       },
    { "date",       NULL,         VALHALLA_META_GRP_TEMPORAL       },
    { "genre",      NULL,         VALHALLA_META_GRP_CLASSIFICATION },
    { "title",      NULL,         VALHALLA_META_GRP_TITLES         },
    { "track",      NULL,         VALHALLA_META_GRP_ORGANIZATIONAL },
    { "year",       NULL,         VALHALLA_META_GRP_TEMPORAL       },

    /* default group */
    { NULL,         NULL,         VALHALLA_META_GRP_MISCELLANEOUS  }
  };
  int i;

  if (!key || !value)
    return;

  for (i = 0; i < ARRAY_NB_ELEMENTS (metagrp); i++)
  {
    char str[32];

    if (metagrp[i].key && strcasecmp (metagrp[i].key, key))
      continue;

    if (fmtname
        && metagrp[i].fmtname && strcasecmp (metagrp[i].fmtname, fmtname))
      continue;

    snprintf (str, sizeof (str), "%s", key);
    my_strtolower (str);
    metadata_add (meta, str, value, metagrp[i].grp);
    break;
  }
}

#define PATTERN_NUMBER "NUM"

static void
parser_decrap_pattern (char *str, const char *bl)
{
  char pattern[64];
  int res, len_s = 0, len_e = 0;
  char *it;

  /* prepare pattern */
  snprintf (pattern, sizeof (pattern), "%%*[^0-9]%%n%s%%n", bl);
  while ((it = strstr (pattern, PATTERN_NUMBER)))
    memcpy (it, "%*u", 3);

  /* search pattern in the string */
  it = str;
  do
  {
    len_s = 0;
    res = sscanf (it, pattern, &len_s, &len_e);
    it += len_s + 1;
  }
  while (!len_e && res != EOF);

  if (len_e <= len_s)
    return;

  /* cleanup */
  memset (it - 1, ' ', len_e - len_s);
}

static void
parser_decrap_blacklist (char **list, char *str)
{
  char **l;

  for (l = list; l && *l; l++)
  {
    size_t size;
    char *p;

    if (strstr (*l, PATTERN_NUMBER))
    {
      parser_decrap_pattern (str, *l);
      continue;
    }

    p = strcasestr (str, *l);
    if (!p)
      continue;

    size = strlen (*l);
    if (!isgraph (*(p + size)) && (p == str || !isgraph (*(p - 1))))
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
    if (!isspace (*it))
    {
      *clean_it = *it;
      clean_it++;
    }
    else if (!isspace (*(it + 1)) && *(it + 1) != '\0')
    {
      *clean_it = ' ';
      clean_it++;
    }
  }

  /* remove spaces after */
  while (clean_it > str && isspace (*(clean_it - 1)))
    clean_it--;
  *clean_it = '\0';

  /* remove spaces before */
  clean_it = clean;
  while (isspace (*clean_it))
    clean_it++;

  strcpy (str, clean_it);
  free (clean);
}

static char *
parser_decrapify (parser_t *parser, const char *file)
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
    if ((unsigned) *it <= 0x7F /* limit to ASCII 7 bits */
        && !isspace (*it)
        && !isalnum (*it))
      *it = ' ';

  parser_decrap_blacklist (parser->bl_list, filename);
  parser_decrap_cleanup (filename);

  res = strdup (filename);
  free (file_tmp);

  valhalla_log (VALHALLA_MSG_VERBOSE, "decrapifier: \"%s\"", res);

  return res;
}

static metadata_t *
parser_metadata_get (parser_t *parser, AVFormatContext *ctx, const char *file)
{
  metadata_t *meta = NULL;
  metadata_t *title_tag = NULL;
  AVMetadataTag *tag = NULL;

  if (!ctx)
    return NULL;

  av_metadata_conv (ctx, NULL, ctx->iformat->metadata_conv);

  while ((tag = av_metadata_get (ctx->metadata,
                                 "", tag, AV_METADATA_IGNORE_SUFFIX)))
    parser_metadata_group (&meta, ctx->iformat->name, tag->key, tag->value);

  /* if necessary, use the filename as title */
  if (parser->decrapifier && metadata_get (meta, "title", 0, &title_tag))
  {
    char *title = parser_decrapify (parser, file);
    if (title)
    {
      metadata_add (&meta, "title", title, VALHALLA_META_GRP_TITLES);
      free (title);
    }
  }

  return meta;
}

#define PROBE_BUF_MIN 2048
#define PROBE_BUF_MAX (1 << 20)

/*
 * This function is fully inspired of (libavformat/utils.c v52.28.0
 * "av_open_input_file()") to probe data in order to test if *ftm argument
 * is the right fmt or not.
 *
 * The original function from avformat "av_probe_input_format2()" is really
 * slow because it probes the fmt of _all_ demuxers for each probe_data
 * buffer. Here, the test is only for _one_ fmt and returns the score
 * (if > score_max) provided by fmt->probe().
 *
 * WARNING: this function depends of some internal behaviours of libavformat
 *          and can be "broken" with future versions of FFmpeg.
 */
static int
parser_probe (AVInputFormat *fmt, const char *file)
{
  FILE *fd;
  int rc = 0;
  int p_size;
  AVProbeData p_data;

  if ((fmt->flags & AVFMT_NOFILE) || !fmt->read_probe)
    return 0;

  fd = fopen (file, "r");
  if (!fd)
    return 0;

  p_data.filename = file;
  p_data.buf = NULL;

  for (p_size = PROBE_BUF_MIN; p_size <= PROBE_BUF_MAX; p_size <<= 1)
  {
    int score;
    int score_max = p_size < PROBE_BUF_MAX ? AVPROBE_SCORE_MAX / 4 : 0;

    p_data.buf = realloc (p_data.buf, p_size + AVPROBE_PADDING_SIZE);
    if (!p_data.buf)
      break;

    p_data.buf_size = fread (p_data.buf, 1, p_size, fd);
    if (p_data.buf_size != p_size) /* EOF is reached? */
      break;

    memset (p_data.buf + p_data.buf_size, 0, AVPROBE_PADDING_SIZE);

    if (fseek (fd, 0, SEEK_SET))
      break;

    score = fmt->read_probe (&p_data);
    if (score > score_max)
    {
      rc = score;
      break;
    }
  }

  if (p_data.buf)
    free (p_data.buf);

  fclose (fd);
  return rc;
}

static metadata_t *
parser_metadata (parser_t *parser, const char *file)
{
  int res;
  const char *name;
  AVFormatContext   *ctx;
  AVInputFormat     *fmt = NULL;
  metadata_t *metadata;

  /*
   * Try a format in function of the suffix.
   * We gain a lot of speed if the fmt is already the right.
   */
  name = suffix_fmt_guess (file);
  if (name)
    fmt = av_find_input_format (name);

  if (fmt)
  {
    int score = parser_probe (fmt, file);
    valhalla_log (VALHALLA_MSG_VERBOSE,
                  "Probe score (%i) [%s] : %s", score, name, file);
    if (!score) /* Bad score? */
      fmt = NULL;
  }

  res = av_open_input_file (&ctx, file, fmt, 0, NULL);
  if (res)
  {
    valhalla_log (VALHALLA_MSG_WARNING,
                  "FFmpeg can't open file (%i) : %s", res, file);
    return NULL;
  }

#if 0
  /*
   * Can be useful in the future in order to detect if the file
   * is audio or video.
   */
  res = av_find_stream_info (ctx);
  if (res < 0)
  {
    valhalla_log (VALHALLA_MSG_WARNING,
                  "FFmpeg can't find stream info: %s", file);
  }
  else
#endif /* 0 */

  metadata = parser_metadata_get (parser, ctx, file);

  av_close_input_file (ctx);
  return metadata;
}

static void *
parser_thread (void *arg)
{
  int res;
  int e;
  void *data = NULL;
  file_data_t *pdata;
  parser_t *parser = arg;

  if (!parser)
    pthread_exit (NULL);

  my_setpriority (parser->priority);

  do
  {
    e = ACTION_NO_OPERATION;
    data = NULL;

    res = fifo_queue_pop (parser->fifo, &e, &data);
    if (res || e == ACTION_NO_OPERATION)
      continue;

    if (e == ACTION_KILL_THREAD)
      break;

    pdata = data;
    if (pdata)
      pdata->meta_parser = parser_metadata (parser, pdata->file);

    file_data_step_increase (pdata, &e);
    dispatcher_action_send (parser->valhalla->dispatcher, e, pdata);
  }
  while (!parser_is_stopped (parser));

  pthread_exit (NULL);
}

int
parser_run (parser_t *parser, int priority)
{
  int i, res = PARSER_SUCCESS;
  pthread_attr_t attr;

  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

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

static inline int
parser_get_list_length (void *list)
{
  void **l = list;
  int n = 0;
  while (l && *l++)
    n++;
  return n;
}

void
parser_bl_keyword_add (parser_t *parser, const char *keyword)
{
  int n;

  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser || !parser->decrapifier || !keyword)
    return;

  n = parser_get_list_length (parser->bl_list) + 1;
  parser->bl_list =
    realloc (parser->bl_list, (n + 1) * sizeof (*parser->bl_list));
  if (!parser->bl_list)
    return;

  parser->bl_list[n] = NULL;
  parser->bl_list[n - 1] = strdup (keyword);
}

fifo_queue_t *
parser_fifo_get (parser_t *parser)
{
  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return NULL;

  return parser->fifo;
}

void
parser_stop (parser_t *parser)
{
  int i;

  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  if (parser_is_stopped (parser))
    return;

  pthread_mutex_lock (&parser->mutex_run);
  parser->run = 0;
  pthread_mutex_unlock (&parser->mutex_run);

  for (i = 0; i < parser->nb; i++)
    fifo_queue_push (parser->fifo, ACTION_KILL_THREAD, NULL);

  for (i = 0; i < parser->nb; i++)
    pthread_join (parser->thread[i], NULL);
}

void
parser_uninit (parser_t *parser)
{
  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  if (parser->bl_list)
  {
    char **bl;
    for (bl = parser->bl_list; *bl; bl++)
      free (*bl);
    free (parser->bl_list);
  }

  fifo_queue_free (parser->fifo);
  pthread_mutex_destroy (&parser->mutex_run);

  free (parser);
}

parser_t *
parser_init (valhalla_t *handle, unsigned int nb, int decrapifier)
{
  parser_t *parser;

  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!handle)
    return NULL;

  parser = calloc (1, sizeof (parser_t));
  if (!parser)
    return NULL;

  if (!nb || nb > ARRAY_NB_ELEMENTS (parser->thread))
    goto err;

  parser->fifo = fifo_queue_new ();
  if (!parser->fifo)
    goto err;

  parser->valhalla = handle;
  parser->nb       = nb;
  parser->decrapifier = !!decrapifier;

  pthread_mutex_init (&parser->mutex_run, NULL);

  return parser;

 err:
  parser_uninit (parser);
  return NULL;
}

void
parser_action_send (parser_t *parser, int action, void *data)
{
  valhalla_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!parser)
    return;

  fifo_queue_push (parser->fifo, action, data);
}
