#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <evhttp.h>

#include "user_conn.h"
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
LIST_CLASS(user_conn_http_request, struct http_request, user_conn_node, false)
LIST_CLASS(http_conn_http_request, struct http_request, http_conn_node, true)

struct http_conn
{
  struct evhttp_connection *evhttp_conn;

  /* The associated user connection.  */
  struct user_conn *user_conn;

  /* List of outstanding requests.  */
  struct http_conn_http_request_list requests;

  /* Used by USER_CONN.  */
  struct list_node user_conn_node;

  /* Number of requests served.  */
  int request_count;

  /* The host this connection corresponds to.  */
  char host[0];
};
LIST_CLASS(user_conn_http_conn, struct http_conn, user_conn_node, false)


/* Creates a new http connection to HOST on behalf of the user
   connection USER_CONN.  Attaches the new HTTP connection to
   USER_CONN->HTTP_CONNS.  */
extern struct http_conn *http_conn_new (const char *host, short port,
					struct user_conn *user_conn);

/* Frees CONN aborting any outstanding requests.  This disconnects
   CONN from the CONN->USER_CONN->HTTP_CONNS list and frees any
   requests.  */
extern void http_conn_free (struct http_conn *conn);

/* This function assumes ownership of HEADERS!  */
extern struct http_request *http_request_new (struct http_conn *conn,
					      const char *url,
					      struct http_headers *headers);

/* Frees REQUEST.  The request must not be outstanding.  (If so,
   you'll have to abort the http connection.)  */
extern void http_request_free (struct http_request *request);

#endif
