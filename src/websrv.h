/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#pragma once

#include <microhttpd.h>

enum MHD_Result websrv_queue_response(struct MHD_Connection *conn,
				      unsigned int status,
				      struct MHD_Response *resp);

typedef void (*websrv_ready_callback_t)(unsigned short port, void *arg);

int websrv_listen(unsigned short port);
void websrv_set_ready_callback(websrv_ready_callback_t callback, void *arg);
void websrv_stop(void);
int websrv_stop_requested(void);
int websrv_set_access_token(const char *token);
