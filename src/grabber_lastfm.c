/*
 * GeeXboX Valhalla: tiny media scanner API.
 * Copyright (C) 2009 Benjamin Zores <ben@geexbox.org>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "grabber_common.h"
#include "grabber_lastfm.h"
#include "metadata.h"
#include "xml_utils.h"
#include "url_utils.h"
#include "utils.h"
#include "logs.h"
#include "md5.h"
#include "list.h"

#define GRABBER_CAP_FLAGS \
  GRABBER_CAP_AUDIO

/*
 * The documentation is available on:
 *  http://www.lastfm.fr/api
 */

#define MAX_LIST_DEPTH      100

#define LASTFM_HOSTNAME     "ws.audioscrobbler.com"
#define LASTFM_LICENSE_KEY  "402d3ca8e9bc9d3cf9b85e1202944ca5"
#define LASTFM_QUERY_SEARCH "http://%s/2.0/?method=album.getinfo&api_key=%s&artist=%s&album=%s"

typedef struct grabber_lastfm_s {
  url_t  *handler;
  list_t *list;
  const metadata_plist_t *pl;
} grabber_lastfm_t;

static const metadata_plist_t lastfm_pl[] = {
  { NULL,                             VALHALLA_METADATA_PL_NORMAL   }
};


static int
grabber_lastfm_get (url_t *handler, char **dl_url,
                    const char *artist, const char *album)
{
  char url[MAX_URL_SIZE];
  url_data_t udata;
  xmlChar *cv;

  xmlDocPtr doc;

  /* proceed with Last.fm search request */
  snprintf (url, sizeof (url), LASTFM_QUERY_SEARCH,
            LASTFM_HOSTNAME, LASTFM_LICENSE_KEY, artist, album);

  vh_log (VALHALLA_MSG_VERBOSE, "Search Request: %s", url);

  udata = vh_url_get_data (handler, url);
  if (udata.status != 0)
    return -1;

  vh_log (VALHALLA_MSG_VERBOSE, "Search Reply: %s", udata.buffer);

  /* parse the XML answer */
  doc = vh_xml_get_doc_from_memory (udata.buffer);
  free (udata.buffer);

  if (!doc)
    return -1;

  cv = vh_xml_get_prop_value_from_tree_by_attr (xmlDocGetRootElement (doc),
                                                "image", "size", "extralarge");
  xmlFreeDoc (doc);
  if (!cv)
    return -1;

  *dl_url = strlen ((const char *) cv) ? strdup ((const char *) cv) : NULL;
  xmlFree (cv);

  return 0;
}

static int
grabber_lastfm_cmp_fct (const void *tocmp, const void *data)
{
  if (!tocmp || !data)
    return -1;

  return strcmp (tocmp, data);
}

static int
grabber_lastfm_check (grabber_lastfm_t *lastfm, const char *cover)
{
  char *data = NULL;

  if (!lastfm->list)
    return -1;

  data = vh_list_search (lastfm->list, cover, grabber_lastfm_cmp_fct);
  if (data)
    return 0;

  vh_list_append (lastfm->list, cover, strlen (cover) + 1);
  return -1;
}

/****************************************************************************/
/* Private Grabber API                                                      */
/****************************************************************************/

static void *
grabber_lastfm_priv (void)
{
  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  return calloc (1, sizeof (grabber_lastfm_t));
}

static int
grabber_lastfm_init (void *priv, const grabber_param_t *param)
{
  grabber_lastfm_t *lastfm = priv;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!lastfm)
    return -1;

  lastfm->list = vh_list_new (MAX_LIST_DEPTH, NULL);
  if (!lastfm->list)
    return -1;

  lastfm->handler = vh_url_new (param->url_ctl);
  lastfm->pl      = param->pl;
  return lastfm->handler ? 0 : -1;
}

static void
grabber_lastfm_uninit (void *priv)
{
  grabber_lastfm_t *lastfm = priv;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  if (!lastfm)
    return;

  vh_url_free (lastfm->handler);
  vh_list_free (lastfm->list);
  free (lastfm);
}

static int
grabber_lastfm_grab (void *priv, file_data_t *data)
{
  grabber_lastfm_t *lastfm = priv;
  const metadata_t *album = NULL, *author = NULL;
  char *artist, *alb;
  char *cover, *url = NULL;
  char name[1024] = { 0 };
  int res, err;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  err = vh_metadata_get (data->meta_parser, "album", 0, &album);
  if (err)
    return -1;

  err = vh_metadata_get (data->meta_parser, "author", 0, &author);
  if (err)
  {
    err = vh_metadata_get (data->meta_parser, "artist", 0, &author);
    if (err)
      return -2;
  }

  if (!album || !author)
    return -3;

  if (!album->value || !author->value)
    return -3;

  /* get HTTP compliant keywords */
  artist = vh_url_escape_string (lastfm->handler, author->value);
  alb = vh_url_escape_string (lastfm->handler, album->value);

  snprintf (name, sizeof (name), "%s-%s", artist, alb);
  cover = vh_md5sum (name);
  /*
   * Check if these keywords were already used for retrieving a cover.
   * If yes, then only the association on the available cover is added.
   * Otherwise, the cover will be searched on Last.fm.
   */
  res = grabber_lastfm_check (lastfm, cover);
  if (!res)
  {
    vh_metadata_add_auto (&data->meta_grabber, VALHALLA_METADATA_COVER,
                          cover, VALHALLA_LANG_UNDEF, lastfm->pl);
    goto out;
  }

  res = grabber_lastfm_get (lastfm->handler, &url, artist, alb);
  if (!res)
  {
    vh_metadata_add_auto (&data->meta_grabber, VALHALLA_METADATA_COVER,
                          cover, VALHALLA_LANG_UNDEF, lastfm->pl);
    vh_file_dl_add (&data->list_downloader, url, cover, VALHALLA_DL_COVER);
    free (url);
  }

 out:
  free (cover);
  free (artist);
  free (alb);

  return res;
}

static void
grabber_lastfm_loop (void *priv)
{
  grabber_lastfm_t *lastfm = priv;

  vh_log (VALHALLA_MSG_VERBOSE, __FUNCTION__);

  /* Hash cover list cleanup */
  vh_list_empty (lastfm->list);
}

/****************************************************************************/
/* Public Grabber API                                                       */
/****************************************************************************/

/* vh_grabber_lastfm_register () */
GRABBER_REGISTER (lastfm,
                  GRABBER_CAP_FLAGS,
                  lastfm_pl,
                  0,
                  grabber_lastfm_priv,
                  grabber_lastfm_init,
                  grabber_lastfm_uninit,
                  grabber_lastfm_grab,
                  grabber_lastfm_loop)
