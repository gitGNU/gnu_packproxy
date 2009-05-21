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

struct http_response *
http_response_new (struct user_conn *user_conn,
		   struct http_request *reply_to)
{
  struct http_response *response = calloc (sizeof (*response), 1);
  if (! response)
    goto err;

  response->buffer = evbuffer_new ();
  if (! response->buffer)
    goto err_with_mem;

  http_message_init (&response->message, HTTP_RESPONSE, user_conn,
		     &reply_to->message);

  return response;

 err_with_mem:
  free (response);
 err:
  return NULL;
}

void
http_response_free (struct http_response *response)
{
  http_message_destroy (&response->message);
  
  evbuffer_free (response->buffer);

  free (response);
}
