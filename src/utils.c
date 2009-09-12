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

#define _GNU_SOURCE
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "valhalla.h"
#include "valhalla_internals.h"
#include "metadata.h"
#include "fifo_queue.h"
#include "utils.h"


void
vh_strtolower (char *str)
{
  if (!str)
    return;

  for (; *str; str++)
    *str = (char) tolower ((int) (unsigned char) *str);
}

char *
vh_strrcasestr (const char *buf, const char *str)
{
  char *ptr, *res = NULL;

  while ((ptr = strcasestr (buf, str)))
  {
    res = ptr;
    buf = ptr + strlen (str);
  }

  return res;
}

int
file_exists (const char *file)
{
  struct stat st;
  return !stat (file, &st);
}

#define BUFFER_SIZE 512

int
file_copy (const char *src, const char *dst)
{
  struct stat st_src, st_dst;
  int fd_src, fd_dst;
  ssize_t r, w;
  char b[BUFFER_SIZE];
  int err, res = 1;

  if (!src || !dst)
    goto end;

  /* ensure that source file exists and is readable */
  err = stat (src, &st_src);
  if (err || !S_ISREG (st_src.st_mode))
    goto end;

  /* ensure that destination file does not already exists */
  err = stat (dst, &st_dst);
  if (!err)
    goto end;

  /* open the corresponding file descriptors */
  fd_src = open (src, O_RDONLY);
  if (fd_src == -1)
    goto end;

  fd_dst = open (dst, O_CREAT | O_WRONLY, 0644);
  if (fd_dst == -1)
    goto end;

  /* proceed with the file copy */
  while ((r = read (fd_src, b, sizeof (b))) > 0)
  {
    w = write (fd_dst, b, r);
    if (w == -1)
      goto end;
  }

  if (!r)
  res = 0;

 end:
  if (fd_src)
    close (fd_src);
  if (fd_dst)
    close (fd_dst);

  return res;
}

void
file_dl_add (file_dl_t **dl,
             const char *url, const char *name, valhalla_dl_t dst)
{
  file_dl_t *it;

  if (!dl || !url || !name || dst >= VALHALLA_DL_LAST || dst < 0)
    return;

  if (!*dl)
  {
    *dl = calloc (1, sizeof (file_dl_t));
    it = *dl;
  }
  else
  {
    it = *dl;
    while (it->next)
      it = it->next;
    it->next = calloc (1, sizeof (file_dl_t));
    it = it->next;
  }

  if (!it)
    return;

  it->url  = strdup (url);
  it->dst  = dst;
  it->name = strdup (name);
}

static void
file_dl_free (file_dl_t *dl)
{
  file_dl_t *dl_n;

  if (!dl)
    return;

  while (dl)
  {
    dl_n = dl->next;
    if (dl->url)
      free (dl->url);
    if (dl->name)
      free (dl->name);
    free (dl);
    dl = dl_n;
  }
}

void
file_data_free (file_data_t *data)
{
  if (!data)
    return;

  if (data->file)
    free (data->file);
  if (data->meta_parser)
    metadata_free (data->meta_parser);
  if (data->meta_grabber)
    metadata_free (data->meta_grabber);
  if (data->list_downloader)
    file_dl_free (data->list_downloader);

  sem_destroy (&data->sem_grabber);

  free (data);
}

void
file_data_step_increase (file_data_t *data, action_list_t *action)
{
  if (!data || !action)
    return;

  switch (data->step)
  {
  case STEP_PARSING:
    data->step++;
    break;

#ifdef USE_GRABBER
  case STEP_GRABBING:
    data->step++;
    switch (*action)
    {
    case ACTION_DB_INSERT_P:
      *action = ACTION_DB_INSERT_G;
      break;

    case ACTION_DB_UPDATE_P:
      *action = ACTION_DB_UPDATE_G;
      break;

    default:
      break;
    }
    break;

  case STEP_DOWNLOADING:
    data->step++;
    break;
#endif /* USE_GRABBER */

  default:
    break;
  }
}

void
file_data_step_continue (file_data_t *data, action_list_t *action)
{
  if (!data || !action)
    return;

#ifdef USE_GRABBER
  if (data->step != STEP_GRABBING)
    return;

  switch (*action)
  {
  case ACTION_DB_INSERT_P:
    *action = ACTION_DB_INSERT_G;
    break;

  case ACTION_DB_UPDATE_P:
    *action = ACTION_DB_UPDATE_G;
    break;

  default:
    break;
  }
#endif /* USE_GRABBER */
}

void
queue_cleanup (fifo_queue_t *queue)
{
  int e;
  void *data;

  fifo_queue_push (queue, ACTION_CLEANUP_END, NULL);

  do
  {
    e = ACTION_NO_OPERATION;
    data = NULL;
    fifo_queue_pop (queue, &e, &data);

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
        file_data_free (data);
      break;
    }
  }
  while (e != ACTION_CLEANUP_END);
}
