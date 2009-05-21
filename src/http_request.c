/* http_request.c - HTTP request processing.
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

#include "http_request.h"
#include "http_conn.h"
#include "user_conn.h"
#include "log.h"

static void
http_request_complete (struct evhttp_request *evrequest, void *arg)
{
  struct http_request *request = arg;

  if (! evrequest)
    {
      log ("%s: Request failed.", request->url);

      http_conn_free (request->http_conn);
      
      return;
    }

  assert (request->evhttp_request == evrequest);

  {
    unsigned char *buffer = EVBUFFER_DATA (evrequest->input_buffer);
    char preview[41];
    int i;
    for (i = 0;
	 i < sizeof (preview) - 1
	   && i < EVBUFFER_LENGTH (evrequest->input_buffer);
	 i ++)
      if (isprint (buffer[i]))
	preview[i] = buffer[i];
      else
	preview[i] = '?';

    preview[i] = 0;

    log ("%s: %zd bytes, `%s%s'",
	 request->url,
	 EVBUFFER_LENGTH (evrequest->input_buffer),
	 preview,
	 i < EVBUFFER_LENGTH (evrequest->input_buffer) ? "..." : "");
  }

  http_request_processed_cb (request);
}

struct http_request *
http_request_new (struct user_conn *user_conn,
		  struct http_conn *http_conn, const char *url,
		  struct http_headers *headers)
{
  assert (http_conn->evhttp_conn);

  int url_len = strlen (url);
  struct http_request *request = calloc (sizeof (*request) + url_len + 1, 1);
  memcpy (request->url, url, url_len + 1);

  request->http_conn = http_conn;

  log ("Request for %s (request: %p; http conn: %p)",
       url, request, http_conn);

  /* Queue the request.  */
  request->evhttp_request = evhttp_request_new (http_request_complete, request);
  if (! request->evhttp_request)
    {
      log ("Failed to allocate request object");
      goto evhttp_request_new_fail;
    }

  request->client_headers = headers;

  /* Add the appropriate headers.  */
  evhttp_add_header (request->evhttp_request->output_headers,
		     "Host", http_conn->host);

  struct http_header *h;
  for (h = headers->head; h; h = h->next)
    if (strcmp (h->key, "Host") != 0)
      {
	evhttp_add_header(request->evhttp_request->output_headers,
			  h->key, h->value);
	log ("Forwarding: %s: %s", h->key, h->value);
      }


  http_conn_http_request_list_enqueue (&http_conn->requests,
				       request);

  evhttp_make_request (http_conn->evhttp_conn, request->evhttp_request,
		       EVHTTP_REQ_GET, url);

  http_conn->request_count ++;

  http_message_init (&request->message, HTTP_REQUEST, user_conn, NULL);

  return request;

 evhttp_request_new_fail:
  free (request);
  return NULL;
}

void
http_request_free (struct http_request *request)
{
  /* REQUEST->HTTP_CONN owns REQUEST->EVHTTP_REQUEST.  It will free
     it.  */

  http_message_destroy (&request->message);

  http_headers_free (request->client_headers);

  /* Unlink.  */
  http_conn_http_request_list_unlink (&request->http_conn->requests, request);

  free (request);
}
