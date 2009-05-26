/* user_conn.c - User connection management.
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
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "user_conn.h"
#include "http_conn.h"
#include "http_request.h"
#include "http_response.h"
#include "http_headers.h"
#include "log.h"
#include "gzip.h"
#include "jpeg.h"
#include "png-support.h"

static void
user_conn_error (struct bufferevent *source, short what, void *arg)
{
  struct user_conn *conn = arg;
  assert (! conn->dead);
  assert (source == conn->event_source);

  log ("event: %d", (int) what);
  switch (what)
    {
    case EVBUFFER_READ:
      log ("read");
      break;
    case EVBUFFER_WRITE:
      log ("write");
      break;
    case EVBUFFER_EOF:
      log ("eof");
      break;
    case EVBUFFER_ERROR:
      log ("error");
      break;
    case EVBUFFER_TIMEOUT:
      log ("timeout");
      break;
    default:
      log ("unknown");
      break;
    }
}

/* Event handler for data on active connections.  */
static void
user_conn_input_available (struct bufferevent *source, void *arg)
{
  struct user_conn *conn = arg;
  assert (! conn->dead);
  assert (source == conn->event_source);

#define DEFAULT_ERROR 501
#define DEFAULT_ERROR_STRING "Unsupported method."

  int do_drain = 0;
  int send_error = 0;
  const char *send_error_string = NULL;
  while (1)
    {
      /* Send any pending error.  */
      if (send_error)
	http_response_new_error (conn, NULL, send_error, send_error_string,
				 true, NULL);
      /* Drain the last read input.  */
      evbuffer_drain (source->input, do_drain);

      send_error = DEFAULT_ERROR;
      send_error_string = DEFAULT_ERROR_STRING;


      /* See if we have a pending command.  */
      char *command = (char *) EVBUFFER_DATA (source->input);

      /* A command ends with the following string.  */
#define EOC "\r\n\r\n"
#define EOC_LEN (sizeof (EOC) - 1)
      char *eoc = memmem (command, EVBUFFER_LENGTH (source->input),
			  EOC, EOC_LEN);
      if (! eoc)
	/* No end of command => no command ready to process.  */
	break;

      /* We got a whole command.  */

      conn->request_count ++;

      int eoc_offset = (intptr_t) eoc - (intptr_t) command;
      do_drain = eoc_offset + EOC_LEN;

      /* NUL terminate the command by replacing the first terminating
	 character with a \0.  */
      *eoc = 0;

      log (BOLD ("request: ") " on user conn %p: `%s'",
	   conn, command);

      /* Process the command.  */

      /* Some clients gratuitous \r\n or \n's at the end of a command.
	 Don't be confused.  */
      while (strncmp (command, "\r\n", 2) == 0)
	command += 2;
      while (*command == '\n')
	command ++;

      /* Extract the method.  */

      /* Ignore any leading white space.  */
      while (*command == ' ')
	command ++;
      const char *verb = command;
      const char *verb_end = strchr (command, ' ');
      if (! verb_end)
	{
	  log ("Garbage request (%s)", command);
	  continue;
	}

      int verb_len = (intptr_t) verb_end - (intptr_t) verb;

      const char *url = verb_end + 1;
      while (*url == ' ')
	url ++;

      char *url_end = strchr (url, ' ');
      if (! url_end)
	{
	  log ("Request (%s) lacks a URL!", command);
	  continue;
	}

      enum http_method method = -1;
      if (verb_len == 3 && memcmp (verb, "GET", 3) == 0)
	method = HTTP_GET;
#if 0
      else if (verb_len == 4 && memcmp (verb, "POST", 4) == 0)
	method = HTTP_POST;
      else if (verb_len == 4 && memcmp (verb, "HEAD", 4) == 0)
	method = HTTP_HEAD;
      else if (verb_len == 3 && memcmp (verb, "PUT", 3) == 0)
	method = HTTP_PUT;
      else if (verb_len == 7 && memcmp (verb, "DELETE", 7) == 0)
	method = HTTP_DELETE;
      else if (verb_len == 7 && memcmp (verb, "OPTIONS", 7) == 0)
	method = HTTP_OPTIONS;
      else if (verb_len == 5 && memcmp (verb, "TRACE", 5) == 0)
	method = HTTP_DELETE;
#endif
      else
	{
	  log ("Request (%s) does not include supported verb!", command);
	  continue;
	}

      char *version = url_end + 1;
      while (*version == ' ')
	version ++;

      enum http_version client_version;
      if (strncmp (version, "HTTP/1.1", 8) == 0)
	client_version = HTTP_11;
      else if (strncmp (version, "HTTP/1.0", 8) == 0)
	client_version = HTTP_10;
      else
	{
	  log ("Request (%s): unknown http version.", command);
	  continue;
	}

      char *request_end = strchr (url, '\n');
      if (! request_end)
	{
	  log ("Request (%s): badly formed request.", command);
	  continue;
	}

      /* Parse the headers.  */
      char *headers = request_end + 1;
      struct http_headers *client_headers = http_headers_new (headers);

      /* Extract the host.  */
      const char *host = http_headers_find (client_headers, "Host");
      if (! host)
	{
	  log ("Request (%s) does not include a Host field (%.10s...)!",
	       command, host);
	  continue;
	}

      /* Try to reuse an existing server connection.  */
      struct http_conn *http_conn;
      for (http_conn = user_conn_http_conn_list_head (&conn->http_conns);
	   http_conn;
	   http_conn = user_conn_http_conn_list_next (http_conn))
	if (! http_conn->close && strcmp (host, http_conn->host) == 0)
	  break;

      if (! http_conn)
	/* Allocate an http connection.  */
	{
	  http_conn = http_conn_new (host, conn);
	  if (! http_conn)
	    {
	      log ("Failed to create http connection.");
	      continue;
	    }
	}

      /* Forward the request.  */
      
      /* Add the appropriate headers.  */
      struct http_headers *request_headers = http_headers_new (NULL);

      /* Forward most client provided headers, e.g., don't forward
	 hop-by-hop headers.  */
      struct http_header *h;
      for (h = client_headers->head; h; h = h->next)
	if (strcmp (h->key, "Connection") == 0)
	  {
	    if (strcmp (h->value, "close") == 0)
	      bufferevent_disable (conn->event_source, EV_READ);
	  }
	else if (strcmp (h->key, "Keep-Alive") != 0
		 && strcmp (h->key, "Public") != 0
		 && strcmp (h->key, "Proxy-Authenticate") != 0
		 && strcmp (h->key, "Transfer-Encoding") != 0
		 && strcmp (h->key, "Upgrade") != 0

		 && strcmp (h->key, "Accept-Encoding") != 0
		 && strcmp (h->key, "Range") != 0
		 && strcmp (h->key, "Proxy-Connection") != 0
		 && strcmp (h->key, "Proxy-Authorization") != 0)
	  {
	    http_headers_add (request_headers, h->key, h->value);
	    log ("Forwarding: %s: %s", h->key, h->value);
	  }
	else
	  log ("Not forwarding: %s: %s", h->key, h->value);

      const char *connection
	= http_headers_find (client_headers, "Connection");
      if (client_version == HTTP_10)
	/* HTTP 1.0 connections are not persistent by default.  */
	{
	  const char *keep_alive
	    = http_headers_find (client_headers, "Keep-Alive");
	  if (connection && strcmp (connection, "Keep-Alive") == 0
	      && keep_alive)
	    /* We can use a persistent connection.  */;
	  else
	    bufferevent_disable (conn->event_source, EV_READ);
	}
      else
	/* HTTP 1.1 connections are persistent by default.  See if the
	   client overrode it.  */
	{
	  if (connection && strcmp (connection, "close") == 0)
	    bufferevent_disable (conn->event_source, EV_READ);
	}

      /* We can't send an absolute URI to an HTTP 1.0 server.
	 However, 1.1 servers will accept Host + resource.  Do that by
	 default.  */
      *url_end = 0;
      const char *resource = url;
      if (strncasecmp (url, "http://", 7) == 0
	  && strncasecmp (url + 7, host, strlen (host)) == 0)
	{
	  resource += 7 + strlen (host);
	  if (*resource == 0)
	    resource = "/";
	  else if (*resource != '/')
	    resource = url;
	}

      struct http_request *request
	= http_request_new (conn, http_conn,
			    resource, HTTP_GET, request_headers, NULL,
			    client_version, client_headers);
      if (! request)
	{
	  log ("Failed to create http request.");
	  http_conn_free (http_conn);
	}

      log ("http conn: %p; request: %p", http_conn, request);

      send_error = 0;
    }
}

/* Forward.  */
static void user_conn_output_buffer_drained (struct bufferevent *output,
					     void *arg);

static struct user_conn_list user_conns;

void
user_conns_dump (void)
{
  int ucs = 0;
  int hcs = 0;
  int reqs = 0;
  int resps = 0;

  struct user_conn *user_conn;
  for (user_conn = user_conn_list_head (&user_conns);
       user_conn;
       user_conn = user_conn_list_next (user_conn))
    {
      printf ("User conn (%d: %p):\n"
	      " origin: %s\n"
	      " buffered input: %d bytes\n"
	      " request count: %d\n",
	      ++ ucs, user_conn,
	      user_conn->ip,
	      EVBUFFER_LENGTH (user_conn->event_source->input),
	      user_conn->request_count);
      if (! (user_conn->event_source->enabled & EV_READ))
	printf (BOLD ("  pending close") "\n");

      struct http_conn *http_conn;
      for (http_conn = user_conn_http_conn_list_head (&user_conn->http_conns);
	   http_conn;
	   http_conn = user_conn_http_conn_list_next (http_conn))
	printf (" connection (%d: %p) to %s\n"
		"  request count: %d\n"
		"  pending close: %d\n",
		++ hcs, http_conn,
		http_conn->host, http_conn->request_count,
		http_conn->close);

      struct http_message *message;
      for (message = user_conn_http_message_list_head (&user_conn->messages);
	   message;
	   message = user_conn_http_message_list_next (message))
	{
	  struct evbuffer *buffer = NULL;
	  switch (message->type)
	    {
	    case HTTP_REQUEST:
	      {
		struct http_request *request
		  = (struct http_request *) message;
		printf ("  request (%d: %p): %s\n",
			++ reqs, request, request->url);
		buffer = request->evhttp_request->input_buffer;

		break;
	      }
	    case HTTP_RESPONSE:
	      {
		struct http_response *response
		  = (struct http_response *) message;
		printf ("  response (%d: %p): %s\n"
			"   ready: %d\n",
			++ resps, response, response->origin,
			response->ready_to_go);
		buffer = response->buffer;
		break;
	      }
	    default:
	      printf ("Unknown message type: %d", message->type);
	      abort ();
	    }

	  printf ("   data (%d): ", EVBUFFER_LENGTH (buffer));
	  char data[80];
	  int i;
	  for (i = 0; i < sizeof (data) && i < EVBUFFER_LENGTH (buffer); i ++)
	    printf ("%c",
		    isprint (EVBUFFER_DATA (buffer)[i])
		    ? EVBUFFER_DATA (buffer)[i]
		    : '?');
	  printf ("\n");
	}
    }
}

struct user_conn *
user_conn_new (int fd, const char *ip)
{
  log ("Creating new session for %s", ip);

  int ip_len = strlen (ip);

  /* Create an event structure and register it.  */
  struct user_conn *user_conn = calloc (sizeof (*user_conn) + ip_len + 1, 1);
  if (! user_conn)
    goto user_conn_alloc_fail;

  memcpy (user_conn->ip, ip, ip_len);

  user_conn->fd = fd;

  user_conn->event_source
    = bufferevent_new (user_conn->fd,
		       user_conn_input_available,
		       user_conn_output_buffer_drained,
		       user_conn_error,
		       user_conn);
  if (! user_conn->event_source)
    goto bufferevent_new_fail;

  /* Reading is disabled by default and writing is enabled.  */
  bufferevent_enable (user_conn->event_source, EV_READ);
  bufferevent_disable (user_conn->event_source, EV_WRITE);

  user_conn_list_enqueue (&user_conns, user_conn);

  return user_conn;

 bufferevent_new_fail:
  free (user_conn);
 user_conn_alloc_fail:
  return NULL;
}

void
user_conn_free (struct user_conn *user_conn)
{
  log ("Closing user connection (%p) from %s.  %d requests.",
       user_conn, user_conn->ip, user_conn->request_count);

  assert (! user_conn->dead);
  user_conn->dead = true;

  {
    /* Make sure that we don't double free.  */
    struct user_conn *uc;
    for (uc = user_conn_list_head (&user_conns);
	 uc; uc = user_conn_list_next (uc))
      if (uc == user_conn)
	break;
    assert (uc);
  }

  bufferevent_free (user_conn->event_source);

  /* Close any extant connections.  */
  struct http_conn *http_conn;
  while ((http_conn = user_conn_http_conn_list_head (&user_conn->http_conns)))
    http_conn_free (http_conn);

  struct http_message *message;
  while ((message = user_conn_http_message_list_head (&user_conn->messages)))
    {
      /* We cleaned up the http connections, which should have freed
	 any requests.  */
      assert (message->type == HTTP_RESPONSE);
      http_message_free (message);
    }

  close (user_conn->fd);

  user_conn_list_unlink (&user_conns, user_conn);

  free (user_conn);
}

static void
user_conn_output_buffer_drained (struct bufferevent *output, void *arg)
{
  struct user_conn *user_conn = arg;

  assert (! user_conn->dead);
  assert ((output->enabled & EV_WRITE));

  /* See if a response is pending.  */
  struct http_message *message
    = user_conn_http_message_list_head (&user_conn->messages);
  if (message && message->type == HTTP_RESPONSE
      && ((struct http_response *) message)->ready_to_go)
    /* Start copying the next response.  */
    {
      struct http_response *response = (struct http_response *) message;

      int len = EVBUFFER_LENGTH (response->buffer);
      log ("sending %d bytes to client", len);
      user_conn->client_out_bytes += len;

      bufferevent_write_buffer (user_conn->event_source, response->buffer);
    }
  else
    {
      /* Disable the copying.  */
      bufferevent_disable (user_conn->event_source, EV_WRITE);

      /* If there are no pending requests or responses and the user
	 side has been closed, destroy the connection.  */
      if (! message
	  && ! (user_conn->event_source->enabled & EV_READ))
	user_conn_free (user_conn);
    }
}

void
user_conn_kick (struct user_conn *user_conn)
{
  /* When calling user_free, it frees any pending connections, which
     may generate error responses, which call this function.  This
     breaks the infernal loop.  */
  if (user_conn->dead)
    return;

  if ((user_conn->event_source->enabled & EV_WRITE))
    /* Already sending.  */
    {
      log ("%p already sending (%x)",
	   user_conn, user_conn->event_source->enabled);
      return;
    }

  struct http_message *message
    = user_conn_http_message_list_head (&user_conn->messages);
  if (! message)
    /* Nothing waiting.  */
    {
      if (! (user_conn->event_source->enabled & EV_READ))
	{
	  log ("%p: read disabled and nothing pending (%x)",
	       user_conn, user_conn->event_source->enabled);
	  user_conn_free (user_conn);
	}
      return;
    }
  if (message->type != HTTP_RESPONSE)
    /* Not a response.  */
    return;

  struct http_response *response = (struct http_response *) message;
  if (response->ready_to_go)
    /* The response is finished.  Queue it up.  */
    {
      user_conn->client_out_bytes += EVBUFFER_LENGTH (response->buffer);
      log ("sending %d bytes to client", EVBUFFER_LENGTH (response->buffer));
      bufferevent_write_buffer (user_conn->event_source, response->buffer);
      bufferevent_enable (user_conn->event_source, EV_WRITE);

      /* bufferevent_write_buffer copies the bytes.  */
      http_response_free (response);
    }
}

static void
encode_compressed_content (struct http_request *request,
			   struct http_response *response,
			   int min_percent, 
			   int deflate_flag)
{
  char *encoding_type = deflate_flag ? "deflate" : "gzip";
  struct evbuffer *compressed
    = evbuffer_gzip (request->evhttp_request->input_buffer, min_percent, 
		     deflate_flag);
  if (compressed)
    {
      log ("compressed (%s): %d -> %d",
	   encoding_type,
	   EVBUFFER_LENGTH (request->evhttp_request->input_buffer),
	   EVBUFFER_LENGTH (compressed));

      evbuffer_drain (request->evhttp_request->input_buffer,
		      EVBUFFER_LENGTH (request->evhttp_request
				       ->input_buffer));
      evbuffer_add_buffer (request->evhttp_request->input_buffer,
			   compressed);
      evbuffer_free (compressed);

      evbuffer_add_printf (response->buffer,
			   "Content-Encoding: %s\r\n", encoding_type);
      log ("Adding: Content-Encoding: %s", encoding_type);
    }
}

void
http_request_processed_cb (struct http_request *request)
{
  /* NB: REQUEST->EVHTTP_REQUEST will disappear when we return.  We
     must copy any data that we would like to preserve.  */

  int we_prefer_deflate = 1;

  struct user_conn *user_conn = request->http_conn->user_conn;

  struct evbuffer *payload = request->evhttp_request->input_buffer;

  struct http_response *response = http_response_new (user_conn, request,
						      request->url);
  struct evbuffer *message = response->buffer;

  log ("%s -> %d: %s (HTTP/%d.%d)",
       request->url,
       request->evhttp_request->response_code,
       request->evhttp_request->response_code_line,
       request->evhttp_request->major,
       request->evhttp_request->minor);

  evbuffer_add_printf (message,
		       "HTTP/%d.%d %d %s\r\n",
		       request->evhttp_request->major,
		       request->evhttp_request->minor,
		       request->evhttp_request->response_code,
		       request->evhttp_request->response_code_line);

  const char *transfer_encoding = NULL;
  const char *content_length = NULL;
  const char *connection = NULL;
  const char *content_encoding = NULL;
  const char *content_type = NULL;

  struct evkeyval *header;
  TAILQ_FOREACH(header, request->evhttp_request->input_headers, next)
    {
      if (strcasecmp (header->key, "transfer-encoding") == 0)
	/* Skip.  */
	{
	  log ("Ignoring %s: %s", header->key, header->value);
	  transfer_encoding = header->value;
	  continue;
	}
      else if (strcasecmp (header->key, "content-length") == 0)
	{
	  log ("Ignoring %s: %s", header->key, header->value);
	  content_length = header->value;
	  continue;
	}
      else if (strcasecmp (header->key, "connection") == 0)
	{
	  log ("Ignoring %s: %s", header->key, header->value);
	  connection = header->value;
	  continue;
	}
      else if (strcasecmp (header->key, "content-encoding") == 0)
	content_encoding = header->value;
      else if (strcasecmp (header->key, "content-type") == 0)
	content_type = header->value;

      /* Don't both sending these headers...  */
      else if (strcasecmp (header->key, "Server") == 0
	       || strcasecmp (header->key, "X-Powered-By") == 0
	       || strcasecmp (header->key, "X-Cnection") == 0)
	{
	  log ("Ignoring %s: %s", header->key, header->value);
	  continue;
	}

      log ("Forwarding: %s: %s", header->key, header->value);

      evbuffer_add_printf (message, "%s: %s\r\n",
			   header->key, header->value);
    }

  if (! (user_conn->event_source->enabled & EV_READ))
    /* The user closed the connection.  Signal that this is the last
       transfer.  */
    evbuffer_add_printf (message, "Connection: close\r\n");

  if (! request->http_conn->close
      && connection && strcmp (connection, "close") == 0)
    request->http_conn->close = true;

  if (EVBUFFER_LENGTH (payload) > 100)
    {
      if (! content_encoding
	  /* gzip adds a 20 byte header.  If we don't have at least 100
	     bytes it's not worth even trying.  */
	  && (! content_type
	      /* Don't bother trying to compress jpeg or png images.  */
	      && strcmp (content_type, "image/jpeg") != 0)
	      && strcmp (content_type, "image/png") != 0)
	/* The data is not encoded and it looks like some sort of text.
	   gzip it!  */
	{
	  const char *accept_encoding
	    = http_headers_find (request->client_headers, "Accept-Encoding");
	  if (accept_encoding)
	    {
	      char *accept_deflate = strstr (accept_encoding, "deflate");
	      char *accept_gzip = strstr (accept_encoding, "gzip");
	      if (accept_deflate && accept_gzip)
		{
		  if (we_prefer_deflate)
		    encode_compressed_content (request, response, 75, 1);
		  else
		    encode_compressed_content (request, response, 75, 0);
		}
	      else if (accept_deflate)
		encode_compressed_content (request, response, 75, 1);
	      else if (accept_gzip)
		encode_compressed_content (request, response, 75, 0);
	      else
		log ("Client refuses gzip encoding: %s", accept_encoding);
	    }
	  else
	    log ("Client refuses gzip encoding: %s", accept_encoding);
	}
      else
	log ("Content-Encoding: %s; length: %d: Content-Type: %s",
	     content_encoding,
	     EVBUFFER_LENGTH (payload),
	     content_type);

      if (content_type
	  && (strcmp (content_type, "image/jpeg") == 0
	      || strcmp (content_type, "image/png") == 0))
	{
	  struct evbuffer *result;
	  if (strcmp (content_type, "image/jpeg") == 0)
	    result = jpeg_recompress (payload, 30);
	  else
	    result = png_recompress (payload, 30);

	  if (result)
	    {
	      log (BOLD ("compressed (%s): %d -> %d (%d%%)"),
		   request->url,
		   EVBUFFER_LENGTH (payload),
		   EVBUFFER_LENGTH (result),
		   (EVBUFFER_LENGTH (result) * 100)
		   / EVBUFFER_LENGTH (payload));

	      if (EVBUFFER_LENGTH (result)
		  < 90 * EVBUFFER_LENGTH (payload) / 100)
		/* Only send if we get at least a 10% size reduction.
		   Why only 10%?  Due to the quality reduction.  */
		{
		  evbuffer_drain (payload, EVBUFFER_LENGTH (payload));
		  evbuffer_add_buffer (payload, result);
		}
	      else
		log ("Too large, using original");

	      evbuffer_free (result);
	    }
	  else
	    log ("Recompression failed");
	}
    }


  /* Add a content-length field.  */
  evbuffer_add_printf (message, "Content-Length: %d\r\n",
		       EVBUFFER_LENGTH (payload));
  log ("Adding: Content-Length: %d", EVBUFFER_LENGTH (payload));

  evbuffer_add_printf (message, "\r\n");

  evbuffer_add_buffer (message, payload);

  struct http_conn *http_conn = request->http_conn;
  http_request_free (request);
  if (http_conn->close)
    http_conn_free (http_conn);

  /* Mark the response as ready to be sent.  */
  response->ready_to_go = true;
  /* Start sending, if appropriate.  */
  user_conn_kick (user_conn);
}
