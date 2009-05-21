/* user_conn.c
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

#include "user_conn.h"
#include "http_conn.h"
#include "http_request.h"
#include "http_response.h"
#include "http_headers.h"
#include "log.h"
#include "gzip.h"
#include "jpeg.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Event handler for data on active connections.  */
static void
user_conn_input (int fd, short event, void *arg)
{
  struct user_conn *conn = arg;
  assert (fd == conn->fd);

  /* Append to the command buffer.  */

  /* The next free slot.  */
  int next = conn->start + conn->len;
  assert (next <= USER_CONN_BUFFER_SIZE);
  /* The number of bytes available.  */
  int space = USER_CONN_BUFFER_SIZE - next;
  if (space == 0)
    /* No space.  */
    {
      if (next > 0)
	/* There is space at the beginning of the buffer.  Move the
	   existing data.  */
	{
	  memmove (&conn->buffer[0], &conn->buffer[next], conn->len);
	  next = conn->len;
	  space = USER_CONN_BUFFER_SIZE - next;
	}
      else
	/* XXX: There is really no space left.  What to do...  */
	{
	  abort ();
	}
    }

  ssize_t s = read (fd, &conn->buffer[next], space);
  if (s < 0)
    {
      log ("read(): %m");
      s = 0;
    }
  log ("Got %d bytes", s);
  conn->client_in_bytes += s;

  conn->len += s;

  if (s == 0)
    /* End of input.  Note: this does not mean that we can free CONN:
       we may still be sending data to the client.  */
    {
      user_conn_deref (conn);
      return;
    }

  /* A command ends with the following string.  */
#define EOC "\r\n\r\n"
#define EOC_LEN (sizeof (EOC) - 1)

  /* We don't want to search the whole buffer.  We already know that
     the end of command string is not present.  We cannot only search
     the newly received data as the end of command could straddle the
     new data.  At most, EOC_LEN - 1 bytes could be in the former
     buffer (otherwise, we would have processed the command last
     time!).  Adjust NEXT appropriately and set SEARCH_SPACE to the
     number of bytes to search.  */
  next = MAX (conn->start, next - EOC_LEN - 1);
  int search_space = conn->len - (next - conn->start);

  do
    {
      char *eoc = memmem (&conn->buffer[next], search_space, EOC, EOC_LEN);
      if (! eoc)
	/* No end of command => no command ready to process.  */
	break;

      conn->request_count ++;

      /* We got a whole command.  NUL terminate it by replacing the
	 first terminating character with a \0.  */
      *eoc = 0;
      int eoc_offset = (intptr_t) eoc - (intptr_t) conn->buffer;

      char *command = &conn->buffer[conn->start];

      int len = conn->len;

      /* Adjust the start and length appropriately.  */
      conn->len -= (eoc_offset + EOC_LEN) - conn->start;

      if (conn->len == 0)
	/* There is nothing that we would have to copy.  Reset
	   CONN->START to 0.*/
	conn->start = 0;
      else
	conn->start = eoc_offset + EOC_LEN;

      log ("command is: `%s' (len: %d -> %d; start: %d)",
	   command, len, conn->len, conn->start);


      /* Process the command.  */

      /* Extract the request.  */
      const char *verb = command;
      const char *verb_end = strchr (command, ' ');
      if (! verb_end)
	{
	  log ("Garbage request (%s)", command);
	  continue;
	}

      int verb_len = (intptr_t) verb_end - (intptr_t) verb;

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
	  struct http_response *response = http_response_new (conn, NULL);
	  evbuffer_add_printf (response->buffer,
			       "HTTP 1.1 501 Unsupported method.\n\r");
	  response->ready_to_go = true;
	  user_conn_kick (conn);
	  
	  continue;
	}

      const char *url = verb_end + 1;
      while (*url == ' ')
	url ++;

      char *url_end = strchr (url, ' ');
      if (! url_end)
	{
	  log ("Request (%s) contains an invalid GET!", command);
	  continue;
	}

      char *request_end = strchr (url, '\n');
      if (! request_end)
	{
	  log ("Request (%s): badly formed request.", command);
	  continue;
	}

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

      /* Allocate an http connection.  */
      struct http_conn *http_conn;
      http_conn = http_conn_new (host, conn);
      if (! http_conn)
	{
	  log ("Failed to create http connection.");
	  continue;
	}

      /* Issue the request.  */
      
      /* Add the appropriate headers.  */
      struct http_headers *request_headers = http_headers_new (NULL);

      /* Forward most client provided headers, e.g., don't forward
	 hop-by-hop headers.  */
      struct http_header *h;
      for (h = client_headers->head; h; h = h->next)
	if (strcmp (h->key, "Connection") == 0)
	  {
	    if (strcmp (h->value, "close") == 0)
	      conn->closed = true;
	  }
	else if (strcmp (h->key, "Keep-Alive") != 0
		 && strcmp (h->key, "Public") != 0
		 && strcmp (h->key, "Proxy-Authenticate") != 0
		 && strcmp (h->key, "Transfer-Encoding") != 0
		 && strcmp (h->key, "Upgrade") != 0)
	  {
	    http_headers_add (request_headers, h->key, h->value);
	    log ("Forwarding: %s: %s", h->key, h->value);
	  }

      *url_end = 0;
      struct http_request *request
	= http_request_new (conn, http_conn,
			    url, HTTP_GET, request_headers, NULL,
			    HTTP_11, client_headers);
      if (! request)
	{
	  log ("Failed to create http request.");
	  http_conn_free (http_conn);
	}
    }
  while (conn->len > 0);
}

/* Forward.  */
static void user_conn_output_buffer_drained (struct bufferevent *output,
					     void *arg);

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
  user_conn->refs = 1;

  event_set (&user_conn->input, fd, EV_READ|EV_PERSIST,
	     user_conn_input, user_conn);

  int ret = event_add (&user_conn->input, NULL /* No timeout.  */);
  if (ret < 0)
    goto event_add_fail;


  user_conn->output = bufferevent_new (user_conn->fd,
				       NULL,
				       user_conn_output_buffer_drained,
				       NULL,
				       user_conn);
  if (! user_conn->output)
    goto bufferevent_new_fail;

  return user_conn;

 bufferevent_new_fail:
  event_del (&user_conn->input);
 event_add_fail:
  free (user_conn);
 user_conn_alloc_fail:  
  return NULL;
}

void
user_conn_free (struct user_conn *user_conn)
{
  log ("Closing user connection from %s.  %d requests.",
       user_conn->ip, user_conn->request_count);

  bufferevent_disable (user_conn->output, EV_WRITE);
  bufferevent_free (user_conn->output);

  /* Close any extant connections.  */
  struct http_conn *http_conn;
  struct http_conn *next
    = user_conn_http_conn_list_head (&user_conn->http_conns);
  while ((http_conn = next))
    {
      next = user_conn_http_conn_list_next (http_conn);
      http_conn_free (http_conn);
    }
  assert (! user_conn_http_message_list_head (&user_conn->messages));

  if (&user_conn->start >= 0)
    event_del (&user_conn->input);

  close (user_conn->fd);

  free (user_conn);
}

void
user_conn_ref_ (struct user_conn *user_conn, const char *caller)
{
  log ("%s bumping ref for %p to %d", caller, user_conn, user_conn->refs + 1);

  assert (user_conn->refs > 0);
  user_conn->refs ++;
}

void
user_conn_deref_ (struct user_conn *user_conn, const char *caller)
{
  log ("%s downing ref for %p to %d", caller, user_conn, user_conn->refs - 1);

  assert (user_conn->refs > 0);
  user_conn->refs --;
  if (user_conn->refs == 0)
    user_conn_free (user_conn);
}


static void
user_conn_output_buffer_drained (struct bufferevent *output, void *arg)
{
  struct user_conn *user_conn = arg;

  assert (user_conn->sending);

  struct http_message *message
    = user_conn_http_message_list_head (&user_conn->messages);
  assert (message);
  assert (message->type == HTTP_RESPONSE);
  http_message_free (message);


  /* See if a response is pending.  */
  message = user_conn_http_message_list_head (&user_conn->messages);
  if (message && message->type == HTTP_RESPONSE
      && ((struct http_response *) message)->ready_to_go)
    /* Start copying the next response.  */
    {
      struct http_response *response = (struct http_response *) message;

      int len = EVBUFFER_LENGTH (response->buffer);
      log ("sending %d bytes to client", len);
      user_conn->client_out_bytes += len;

      bufferevent_write_buffer (user_conn->output, response->buffer);
    }
  else
    {
      /* Disable the copying.  */
      bufferevent_disable (user_conn->output, EV_WRITE);
      user_conn->sending = false;
      user_conn_deref (user_conn);

      if (! message && user_conn->closed)
	/* The user closed the connection.  */
	user_conn_free (user_conn);
    }
}

void
user_conn_kick (struct user_conn *user_conn)
{
  if (user_conn->sending)
    /* Already sending.  */
    return;

  struct http_message *message
    = user_conn_http_message_list_head (&user_conn->messages);
  if (! message)
    /* Nothing waiting.  */
    return;
  if (message->type != HTTP_RESPONSE)
    /* Not a response.  */
    return;

  struct http_response *response = (struct http_response *) message;
  if (response->ready_to_go)
    /* The response is finished.  Queue it up.  */
    {
      user_conn_ref (user_conn);
      user_conn->sending = true;

      user_conn->client_out_bytes += EVBUFFER_LENGTH (response->buffer);
      log ("sending %d bytes to client", EVBUFFER_LENGTH (response->buffer));
      bufferevent_write_buffer (user_conn->output, response->buffer);
      bufferevent_enable (user_conn->output, EV_WRITE);
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

  struct http_response *response = http_response_new (user_conn, request);
  struct evbuffer *message = response->buffer;

  log ("%s: response code line: `%s'",
       request->url,
       request->evhttp_request->response_code_line);

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
	  transfer_encoding = header->value;
	  continue;
	}
      else if (strcasecmp (header->key, "content-length") == 0)
	{
	  content_length = header->value;
	  continue;
	}
      else if (strcasecmp (header->key, "connection") == 0)
	{
	  connection = header->value;
	  continue;
	}
      else if (strcasecmp (header->key, "content-encoding") == 0)
	content_encoding = header->value;
      else if (strcasecmp (header->key, "content-type") == 0)
	content_type = header->value;

      log ("Forwarding: %s: %s", header->key, header->value);

      evbuffer_add_printf (message, "%s: %s\r\n",
			   header->key, header->value);
    }

  if (user_conn->closed)
    /* The user closed the connection.  Signal that this is the last
       transfer.  */
    evbuffer_add_printf (message, "Connection: close\r\n",
			 header->key, header->value);

  bool connection_closed = false;
  if (connection && strcmp (connection, "close") == 0)
    connection_closed = true;

  if (EVBUFFER_LENGTH (payload) > 100)
    {
      if (! content_encoding
	  /* gzip adds a 20 byte header.  If we don't have at least 100
	     bytes it's not worth even trying.  */
	  && (! content_type
	      /* Don't bother trying to compress jpeg images.  */
	      || (strcmp (content_type, "image/jpeg") != 0)))
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

      if (content_type && strcmp (content_type, "image/jpeg") == 0)
	{
	  struct evbuffer *result = jpeg_recompress (payload, 30);
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
	}
    }


  /* Add a content-length field.  */
  evbuffer_add_printf (message, "Content-Length: %d\r\n",
		       EVBUFFER_LENGTH (payload));
  log ("Adding: Content-Length: %d", EVBUFFER_LENGTH (payload));

  evbuffer_add_printf (message, "\r\n");

  evbuffer_add_buffer (message, payload);

  if (connection_closed)
    http_conn_free (request->http_conn);
  else
    http_request_free (request);

  /* Mark the response as ready to be sent.  */
  response->ready_to_go = true;
  /* Start sending, if appropriate.  */
  user_conn_kick (user_conn);
}
