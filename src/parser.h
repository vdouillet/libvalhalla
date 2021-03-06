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

#ifndef VALHALLA_PARSER_H
#define VALHALLA_PARSER_H

#include "fifo_queue.h"

typedef struct parser_s parser_t;

enum parser_errno {
  PARSER_ERROR_HANDLER = -2,
  PARSER_ERROR_THREAD  = -1,
  PARSER_SUCCESS       =  0,
};

#define PARSER_NUMBER_DEF 2


int vh_parser_run (parser_t *parser, int priority);
void vh_parser_pause (parser_t *parser);
fifo_queue_t *vh_parser_fifo_get (parser_t *parser);
void vh_parser_stop (parser_t *parser, int f);
void vh_parser_uninit (parser_t *parser);
parser_t *vh_parser_init (valhalla_t *handle,
                          unsigned int nb, unsigned int decrapifier);

void vh_parser_bl_keyword_add (parser_t *parser, const char *keyword);

void vh_parser_action_send (parser_t *parser,
                            fifo_queue_prio_t prio, int action, void *data);

#endif /* VALHALLA_PARSER_H */
