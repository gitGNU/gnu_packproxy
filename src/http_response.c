/* http_response.c - http response implementation.
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

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <stdlib.h>

#include "http_response.h"
#include "http_request.h"
#include "user_conn.h"
#include "log.h"

static struct http_response *
http_response_alloc (struct user_conn *user_conn,
		     struct http_request *reply_to,
		     const char *origin)
{
  if (! origin)
    origin = "unknown";

  int origin_len = strlen (origin);
  struct http_response *response
    = calloc (sizeof (*response) + origin_len + 1, 1);
  if (! response)
    return NULL;

  memcpy (response->origin, origin, origin_len);

  response->buffer = evbuffer_new ();
  if (! response->buffer)
    {
      free (response);
      return NULL;
    }

  http_message_init (&response->message, HTTP_RESPONSE, user_conn,
		     &reply_to->message);

  return response;
}

struct http_response *
http_response_new (struct user_conn *user_conn,
		   struct http_request *reply_to,
		   const char *origin)
{
  struct http_response *response
    = http_response_alloc (user_conn, reply_to,
			   origin ?: reply_to ? reply_to->url : NULL);
  if (! response)
    return NULL;

  return response;
}

struct http_response *
http_response_new_error (struct user_conn *user_conn,
			 struct http_request *reply_to,
			 int status_code, const char *status_string,
			 bool close,
			 const char *origin)
{
  struct http_response *response
    = http_response_alloc (user_conn, reply_to,
			   origin ?: reply_to ? reply_to->url : NULL);
  if (! response)
    return NULL;

  log ("Sending a %d %s on user conn %p",
       status_code, status_string, user_conn);

  /* The status line.  */
  evbuffer_add_printf (response->buffer,
		       "HTTP 1.1 %d %s\r\n",
		       status_code, status_string);

  /* Appropriate headers.  */
  if (close)
    {
      bufferevent_disable (user_conn->event_source, EV_READ);
      evbuffer_add_printf (response->buffer, "Connection: close\r\n");
    }

  evbuffer_add_printf (response->buffer, "Content-Length: 0\r\n");

  evbuffer_add_printf (response->buffer, "\r\n");

  if (reply_to)
    http_request_free (reply_to);
  response->ready_to_go = true;
  user_conn_kick (user_conn);

  return response;
}

void
http_response_free (struct http_response *response)
{
  log ("Destroying response %s", response->origin);

  http_message_destroy (&response->message);
  
  evbuffer_free (response->buffer);

  free (response);
}
