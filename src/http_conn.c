#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "http_conn.h"
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

  /* REQUEST->HTTP_CONN owns EVREQUEST!  When we return, it will be
     deallocated.  Make sure we don't access it again.  */
  request->evhttp_request = NULL;
}

struct http_request *
http_request_new (struct http_conn *http_conn, const char *url, char *headers)
{
  assert (http_conn->evhttp_conn);
  assert (http_conn->user_conn);

  int url_len = strlen (url);
  struct http_request *request = calloc (sizeof (*request) + url_len + 1, 1);
  memcpy (request->url, url, url_len);
  request->url[url_len] = 0;

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

  /* Add the appropriate headers.  */
  evhttp_add_header (request->evhttp_request->output_headers,
		     "Host", http_conn->host);
  char *colon;
  while (headers && (colon = strchr (headers, ':')))
    {
      char *key = headers;

      *colon = '\0';
      headers = colon + 1;
      if (*headers == ' ')
	headers ++;

      char *value = headers;

      char *end = strstr (headers, "\r\n");
      if (end)
	{
	  *end = '\0';
	  headers = end + 2;
	}
      else
	headers = NULL;

      evhttp_add_header(request->evhttp_request->output_headers, key, value);
      log ("Forwarding: %s: %s", key, value);
    }

  http_conn_http_request_list_enqueue (&http_conn->requests,
				       request);
  user_conn_http_request_list_enqueue (&http_conn->user_conn->requests,
				       request);

  evhttp_make_request (http_conn->evhttp_conn, request->evhttp_request,
		       EVHTTP_REQ_GET, url);

  http_conn->request_count ++;

  return request;

 evhttp_request_new_fail:
  free (request);
  return NULL;
}

void
http_request_free (struct http_request *request)
{
  /* There is no way to abort an outstanding request unless the whole
     connection is torn down...  */
  assert (! request->evhttp_request);

  if (request->data)
    {
      evbuffer_free (request->data);
      request->data = NULL;
    }

  /* Unlink.  */
  http_conn_http_request_list_unlink (&request->http_conn->requests, request);
  user_conn_http_request_list_unlink (&request->http_conn->user_conn->requests,
				      request);

  free (request);
}

struct http_conn *
http_conn_new (const char *host, short port,
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

  conn->evhttp_conn = evhttp_connection_new (host, port);
  if (! conn->evhttp_conn)
    {
      log ("Cannot establish connection to %s:%d\n", host, port);
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
