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

#ifndef VALHALLA_METADATA
#define VALHALLA_METADATA

#include "valhalla.h"

typedef struct metadata_s {
  struct metadata_s *next;
  char *name;
  char *value;
  valhalla_meta_grp_t group;
} metadata_t;

#define METADATA_IGNORE_SUFFIX (1 << 0)

int metadata_get (metadata_t *meta,
                  const char *name, int flags, metadata_t **tag);
void metadata_free (metadata_t *meta);
void metadata_add (metadata_t **meta, const char *name,
                   const char *value, valhalla_meta_grp_t group);

#endif /* VALHALLA_METADATA */