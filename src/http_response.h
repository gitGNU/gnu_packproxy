/* http_response.h - http response implementation.
   Copyright (C) 2009 Neal H. Walfield <neal@gnu.org>.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Library General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.  */

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>

#include "http_message.h"

/* Forward.  */
struct http_request;

struct http_response
{
  struct http_message message;

  /* Whether the response is ready to be sent.  */
  bool ready_to_go;

  /* The response.  */
  struct evbuffer *buffer;
};

/* Create an http response.  The response is initially empty.  If
   REPLY_TO is not NULL, then this is in response to the message
   REPLY_TO and the new response is enqueued on USER_CONN
   appropriately.  Otherwise, the response is appended to the end of
   USER_CONN message queue.  */
extern struct http_response *http_response_new (struct user_conn *user_conn,
						struct http_request *reply_to);

/* Free the response unlinking it from its associated USER_CONN.  */
extern void http_response_free (struct http_response *response);

#endif
