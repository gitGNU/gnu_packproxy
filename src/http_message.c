/* http_message.c - HTTP message management.
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

#include <stdlib.h>

#include "http_message.h"
#include "http_request.h"
#include "http_response.h"
#include "user_conn.h"

void
http_message_init (struct http_message *message,
		   enum http_message_type type,
		   struct user_conn *user_conn,
		   struct http_message *insert_after)
{
  message->user_conn = user_conn;
  message->type = type;
  if (insert_after)
    user_conn_http_message_list_insert_after (&user_conn->messages,
					      message, insert_after);
  else
    user_conn_http_message_list_enqueue (&user_conn->messages, message);
}

void
http_message_destroy (struct http_message *message)
{
  user_conn_http_message_list_unlink (&message->user_conn->messages, message);
}

void
http_message_free (struct http_message *message)
{
  switch (message->type)
    {
    case HTTP_REQUEST:
      http_request_free ((struct http_request *) message);
      break;

    case HTTP_RESPONSE:
      http_response_free ((struct http_response *) message);
      break;

    default:
      abort ();
    }
}
