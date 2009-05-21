/* http_message.h - HTTP message management.
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

#ifndef HTTP_MESSAGE_H
#define HTTP_MESSAGE_H

#include "list.h"

enum http_message_type
  {
    HTTP_REQUEST = 1,
    HTTP_RESPONSE,
  };

/* This is embedded in the head of struct http_response and struct
   http_request's.  */
struct http_message
{
  enum http_message_type type;
  struct list_node node;
  struct user_conn *user_conn;

  /* The rest of this structure, except that unboxed recursive data
    structures don't really work in C...
    
    union
    {
      struct http_response response;
      struct http_request request;
    };
   */
};
LIST_CLASS(user_conn_http_message, struct http_message, node, true)

/* Initializes the message structure MESSAGE including settings its
   type and linking it to USER_CONN's message list.  If INSERT_AFTER
   is NULL, it is appeneded.  */
void http_message_init (struct http_message *message,
			enum http_message_type type,
			struct user_conn *user_conn,
			struct http_message *insert_after);

/* Dual to http_message_init.  In particular, unlinks MESSAGE from its
   USER_CONN's message list.  */
extern void http_message_destroy (struct http_message *message);


/* Free the message MESSAGE by calling either http_response_free or
   http_request_free as appropriate.  */
extern void http_message_free (struct http_message *message);

#endif
