/* http_request.h
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

#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>

#include "http_headers.h"
#include "list.h"

enum http_version
  {
    HTTP_10,
    HTTP_11,
  };

struct http_request
{
  struct evhttp_request *evhttp_request;

  enum http_version client_version;
  /* The client's HTTP headers.  */
  struct http_headers *client_headers;

  /* The returned data.  */
  struct evbuffer *data;

  /* The http connection that was used.  */
  struct http_conn *http_conn;

  struct list_node http_conn_node;
  struct list_node user_conn_node;

  char url[0];
};
LIST_CLASS(user_conn_http_request, struct http_request, user_conn_node, true)
LIST_CLASS(http_conn_http_request, struct http_request, http_conn_node, true)

/* This function assumes ownership of HEADERS!  */
extern struct http_request *http_request_new (struct http_conn *conn,
					      const char *url,
					      struct http_headers *headers);

/* Frees REQUEST.  The request must not be outstanding.  (If so,
   you'll have to abort the http connection.)  */
extern void http_request_free (struct http_request *request);

#endif
