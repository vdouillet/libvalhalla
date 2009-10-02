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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h>

#include "valhalla.h"
#include "utils.h"
#include "url_utils.h"
#include "logs.h"


static size_t
url_buffer_get (void *ptr, size_t size, size_t nmemb, void *data)
{
  size_t realsize = size * nmemb;
  url_data_t *mem = (url_data_t *) data;

  mem->buffer = realloc (mem->buffer, mem->size + realsize + 1);
  if (mem->buffer)
  {
    memcpy (&(mem->buffer[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = 0;
  }

  return realsize;
}

url_t *
vh_url_new (void)
{
  CURL *curl;

  curl = curl_easy_init ();
  return (url_t *) curl;
}

void
vh_url_free (url_t *url)
{
  if (url)
    curl_easy_cleanup ((CURL *) url);
}

void
vh_url_global_init (void)
{
  curl_global_init (CURL_GLOBAL_DEFAULT);
}

void
vh_url_global_uninit (void)
{
  curl_global_cleanup ();
}

url_data_t
vh_url_get_data (url_t *handler, char *url)
{
  url_data_t chunk;
  CURL *curl = (CURL *) handler;

  chunk.buffer = NULL; /* we expect realloc(NULL, size) to work */
  chunk.size = 0; /* no data at this point */
  chunk.status = CURLE_FAILED_INIT;

  if (!curl || !url)
    return chunk;

  curl_easy_setopt (curl, CURLOPT_URL, url);
  curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, url_buffer_get);
  curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *) &chunk);
  curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 5);

  chunk.status = curl_easy_perform (curl);

  return chunk;
}

char *
vh_url_escape_string (url_t *handler, const char *buf)
{
  CURL *curl = (CURL *) handler;

  if (!curl || !buf)
    return NULL;

  return curl_easy_escape (curl, buf, strlen (buf));
}

int
vh_url_save_to_disk (url_t *handler, char *src, char *dst)
{
  url_data_t data;
  int n, fd;
  CURL *curl = (CURL *) handler;

  if (!curl || !src || !dst)
    return 1;

  /* no need to download again an already existing file */
  if (vh_file_exists (dst))
    return 1;

  valhalla_log (VALHALLA_MSG_VERBOSE, "Saving %s to %s", src, dst);

  data = vh_url_get_data (curl, src);
  if (data.status != CURLE_OK)
  {
    valhalla_log (VALHALLA_MSG_WARNING,
                  "Unable to download requested file");
    return 1;
  }

  fd = open (dst, O_WRONLY | O_CREAT, 0666);
  if (fd < 0)
  {
    valhalla_log (VALHALLA_MSG_WARNING,
              "Unable to open stream to save file");

    free (data.buffer);
    return 1;
  }

  n = write (fd, data.buffer, data.size);
  free (data.buffer);

  return 0;
}
