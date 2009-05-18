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
#include "log.h"
#include "gzip.h"

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

      /* Extract the URL.  */
      if (strncmp (command, "GET ", 4) != 0)
	{
	  log ("Request (%s) does not include a proper GET!", command);
	  continue;
	}
      char *url = command + 4;
      while (*url == ' ')
	url ++;

      char *url_end = strchr (url, ' ');
      if (! url_end)
	{
	  log ("Request (%s) contains an invalid GET!", command);
	  continue;
	}

      char *get_end = strchr (url, '\r');
      if (! get_end || get_end[1] != '\n')
	{
	  log ("Request (%s): badly formed GET.", command);
	  continue;
	}

      /* Extract the host.  */
      const char *host = &get_end[2];
      if (strncmp (host, "Host: ", 6) != 0)
	{
	  log ("Request (%s) does not include a Host field (%.10s...)!",
	       command, host);
	  continue;
	}
      host += 6;
      while (*host == ' ')
	host ++;
      char *host_end = strchr (host, '\r');
      char *headers;
      if (! host_end)
	{
	  host_end = eoc;
	  headers = NULL;
	}
      else
	{
	  headers = &host_end[2];
	}

      *url_end = '\0';
      *host_end = '\0';

      /* Allocate an http connection.  */
      struct http_conn *http_conn;
      http_conn = http_conn_new (host, 80, conn);
      if (! http_conn)
	{
	  log ("Failed to create http connection.");
	  continue;
	}

      /* Issue the request.  */
      struct http_request *request = http_request_new (http_conn, url, headers);
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
  assert (! user_conn_http_request_list_head (&user_conn->requests));

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

  struct http_request *request
    = user_conn_http_request_list_head (&user_conn->requests);
  assert (request);
  http_request_free (request);

  request = user_conn_http_request_list_head (&user_conn->requests);
  if (request && request->data)
    /* Start copying the next buffer.  */
    {
      log ("%s: sending %d bytes to client",
	   request->url, EVBUFFER_LENGTH (request->data));
      user_conn->client_out_bytes += EVBUFFER_LENGTH (request->data);
      bufferevent_write_buffer (user_conn->output, request->data);
    }
  else
    {
      /* Disable the copying.  */
      bufferevent_disable (user_conn->output, EV_WRITE);
      user_conn_deref (user_conn);
    }
}

void
http_request_processed_cb (struct http_request *request)
{
  /* NB: REQUEST->EVHTTP_REQUEST will disappear when we return.  We
     must copy any data that we would like to preserve.  */

  struct user_conn *user_conn = request->http_conn->user_conn;

  assert (! request->data);
  request->data = evbuffer_new ();

  log ("%s: response code line: `%s'",
       request->url,
       request->evhttp_request->response_code_line);

  evbuffer_add_printf (request->data,
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

      evbuffer_add_printf (request->data, "%s: %s\r\n",
			   header->key, header->value);
    }

  if (! content_encoding
      /* gzip adds a 20 byte header.  If we don't have at least 100
	 bytes it's not worth even trying.  */
      && EVBUFFER_LENGTH (request->evhttp_request->input_buffer) > 100
      && content_type
      && (strncmp (content_type, "text/", 5) == 0
	  || strcmp (content_type, "application/x-javascript") == 0))
    /* The data is not encoded and it looks like some sort of text.
       gzip it!  */
    {
      const char *accept_encoding
	= evhttp_find_header (request->evhttp_request->output_headers,
			      "Accept-Encoding");
      if (accept_encoding && strstr (accept_encoding, "gzip"))
	/* The client (appears to) accepts gzip encodings.  */
	{
	  struct evbuffer *gzipped
	    = evbuffer_gzip (request->evhttp_request->input_buffer);
	  if (gzipped)
	    {
	      log ("gzipped: %d -> %d",
		   EVBUFFER_LENGTH (request->evhttp_request->input_buffer),
		   EVBUFFER_LENGTH (gzipped));

	      evbuffer_drain (request->evhttp_request->input_buffer,
			      EVBUFFER_LENGTH (request->evhttp_request
					       ->input_buffer));
	      evbuffer_add_buffer (request->evhttp_request->input_buffer,
				   gzipped);
	      evbuffer_free (gzipped);
	    }

	  evbuffer_add_printf (request->data, "Content-Encoding: gzip\r\n");
	  log ("Adding: Content-Encoding: gzip");
	}
    }


  /* Add a content-length field if there was none.  */
  evbuffer_add_printf (request->data, "Content-Length: %d\r\n",
		       EVBUFFER_LENGTH (request->evhttp_request->input_buffer));
  log ("Adding: Content-Length: %d",
       EVBUFFER_LENGTH (request->evhttp_request->input_buffer));

  evbuffer_add_printf (request->data, "\r\n");

  evbuffer_add_buffer (request->data, request->evhttp_request->input_buffer);

  if (request == user_conn_http_request_list_head (&user_conn->requests))
    /* We must answer requests in the order that we received them.
       This is the next request to answer.  Start copying the
       data.  */
    {
      user_conn_ref (user_conn);

      user_conn->client_out_bytes += EVBUFFER_LENGTH (request->data);
      log ("%s: sending %d bytes to client",
	   request->url, EVBUFFER_LENGTH (request->data));
      bufferevent_write_buffer (user_conn->output, request->data);
      bufferevent_enable (user_conn->output, EV_WRITE);
    }

  if (connection && strcasecmp (connection, "close") == 0)
    /* The server is closing the connection.  */
    /* XXX: Do something intelligent.  */;
}
