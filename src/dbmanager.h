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

#ifndef VALHALLA_DBMANAGER_H
#define VALHALLA_DBMANAGER_H

typedef struct dbmanager_s dbmanager_t;

enum dbmanager_errno {
  DBMANAGER_ERROR_HANDLER = -2,
  DBMANAGER_ERROR_THREAD  = -1,
  DBMANAGER_SUCCESS       =  0,
};

int dbmanager_run (dbmanager_t *dbmanager, int priority);
void dbmanager_cleanup (dbmanager_t *dbmanager);
void dbmanager_stop (dbmanager_t *dbmanager);
void dbmanager_uninit (dbmanager_t *dbmanager);
dbmanager_t *dbmanager_init (valhalla_t *handle,
                             const char *db, unsigned int commit_int);

void dbmanager_action_send (dbmanager_t *dbmanager, int action, void *data);


int dbmanager_db_metalist_get (dbmanager_t *dbmanager,
                               valhalla_db_item_t *search,
                               valhalla_db_restrict_t *restriction,
                               int (*select_cb) (void *data,
                                                 valhalla_db_metares_t *res),
                               void *data);

int dbmanager_db_filelist_get (dbmanager_t *dbmanager,
                               valhalla_file_type_t filetype,
                               valhalla_db_restrict_t *restriction,
                               int (*select_cb) (void *data,
                                                 valhalla_db_fileres_t *res),
                               void *data);

int dbmanager_db_file_get (dbmanager_t *dbmanager,
                           int64_t id, const char *path,
                           valhalla_db_restrict_t *restriction,
                           valhalla_db_filemeta_t **res);

#endif /* VALHALLA_DBMANAGER_H */