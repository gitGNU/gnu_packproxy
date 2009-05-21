/* http_conn.c - HTTP connection management.
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
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "http_conn.h"
#include "user_conn.h"
#include "log.h"

struct http_conn *
http_conn_new (const char *host,
	       struct user_conn *user_conn)
{
  log ("New http connection to %s on behalf of %s",
       host, user_conn->ip);

  int host_len = strlen (host);

  struct http_conn *conn = calloc (sizeof (*conn) + host_len + 1, 1);
  if (! conn)
    return NULL;

  memcpy (conn->host, host, host_len + 1);

  conn->user_conn = user_conn;

  char *h = (char *) host;
  int p = 80;
  char *port = strchr (host, ':');
  if (port)
    {
      p = atoi (port + 1);
      if (! p)
	p = 80;
      else
	{
	  int len = (uintptr_t) port - (uintptr_t) host;
	  h = alloca (len + 1);
	  memcpy (h, host, len);
	  h[len] = 0;
	}
    }

  conn->evhttp_conn = evhttp_connection_new (h, p);
  if (! conn->evhttp_conn)
    {
      log ("Cannot establish connection to %s:%d\n", h, p);
      goto evhttp_connection_new_fail;
    }

  user_conn_http_conn_list_enqueue (&user_conn->http_conns, conn);

  return conn;

 evhttp_connection_new_fail:
  free (conn);
  return NULL;
}

void
http_conn_free (struct http_conn *http_conn)
{
  log ("Closing http connection to %s.  %d requests.",
       http_conn->host, http_conn->request_count);

  /* First, we close the http connection.  This aborts any outstanding
     requests (but without signalling an error).  Thus, we next go
     through and free the outstanding requests.  */
  evhttp_connection_free (http_conn->evhttp_conn);

  struct http_request *request;
  struct http_request *next;
  next = http_conn_http_request_list_head (&http_conn->requests);
  while ((request = next))
    {
      next = http_conn_http_request_list_next (request);

      /* The request was aborted by evhttp_connection_free.  */
      request->evhttp_request = NULL;
      http_request_free (request);
    }

  user_conn_http_conn_list_unlink (&http_conn->user_conn->http_conns,
				   http_conn);
  free (http_conn);
}
