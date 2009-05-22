/*
 * Copyright (c) 2002-2006 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define HAVE_SYS_TIME_H
#define HAVE_GETADDRINFO
#define HAVE_STRSEP
#define HAVE_GETNAMEINFO

#include "log.h"
// #define event_debug(x) log x
#define event_debug(x)
#define event_warn log 
#define event_err(err, ...) do { log (__VA_ARGS__); exit (err); } while (0)

#include <sys/param.h>
#include <sys/types.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_IOCCOM_H
#include <sys/ioccom.h>
#endif

#ifndef WIN32
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include <sys/tree.h>
#include <sys/queue.h>

#ifndef WIN32
#include <netinet/in.h>
#include <netdb.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32
#include <syslog.h>
#endif
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#undef timeout_pending
#undef timeout_initialized

#include "strlcpy-internal.h"
#include "event.h"
#include "evhttp.h"
#include "log.h"
#include "http-internal.h"

#ifndef HAVE_GETADDRINFO
struct addrinfo {
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	size_t ai_addrlen;
	struct sockaddr *ai_addr;
	struct addrinfo *ai_next;
};
static int
fake_getaddrinfo(const char *hostname, struct addrinfo *ai)
{
	struct hostent *he;
	he = gethostbyname(hostname);
	if (!he)
		return (-1);
	ai->ai_family = he->h_addrtype;
	ai->ai_socktype = SOCK_STREAM;
	ai->ai_protocol = 0;
	ai->ai_addrlen = he->h_length;
	if (NULL == (ai->ai_addr = malloc(ai->ai_addrlen)))
		return (-1);
	memcpy(ai->ai_addr, &he->h_addr_list[0], ai->ai_addrlen);
	ai->ai_next = NULL;
	return (0);
}
static void
fake_freeaddrinfo(struct addrinfo *ai)
{
	free(ai->ai_addr);
}
#endif

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

static int
event_make_socket_nonblocking(int fd)
{

#ifdef WIN32
	{
		unsigned long nonblocking = 1;
		ioctlsocket(fd, FIONBIO, (unsigned long*) &nonblocking);
	}
#else
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
		event_warn("fcntl(O_NONBLOCK)");
		return -1;
	}
#endif
	return 0;
}

extern int debug;

static int make_socket_ai(int should_bind, struct addrinfo *);
static int make_socket(int should_bind, const char *, u_short);
static void name_from_addr(struct sockaddr *, socklen_t, char **, char **);
static int evhttp_associate_new_request_with_connection(
	struct evhttp_connection *evcon);
static void evhttp_connection_start_detectclose(
	struct evhttp_connection *evcon);
static void evhttp_connection_stop_detectclose(
	struct evhttp_connection *evcon);
static void evhttp_request_dispatch(struct evhttp_connection* evcon);

void evhttp_read(int, short, void *);
void evhttp_write(int, short, void *);

#ifndef HAVE_STRSEP
static char *
strsep(char **s, const char *del)
{
	char *d, *tok;
	if (!s || !*s)
		return NULL;
	tok = *s;
	d = strstr(tok, del);
	if (d)
		*s = d + strlen(del);
	else
		*s = NULL;
	return tok;
}
#endif

static const char *
html_replace(char ch)
{
	static char buf[2];
	
	switch (ch) {
	case '<':
		return "&lt;";
	case '>':
		return "&gt;";
	case '"':
		return "&quot;";
	case '\'':
		return "&#039;";
	case '&':
		return "&amp;";
	default:
		break;
	}

	/* Echo the character back */
	buf[0] = ch;
	buf[1] = '\0';
	
	return buf;
}

/*
 * Replaces <, >, ", ' and & with &lt;, &gt;, &quot;,
 * &#039; and &amp; correspondingly.
 *
 * The returned string needs to be freed by the caller.
 */

char *
evhttp_htmlescape(const char *html)
{
	int i, new_size = 0, old_size = strlen(html);
	char *escaped_html, *p;
	
	for (i = 0; i < old_size; ++i)
		new_size += strlen(html_replace(html[i]));

	p = escaped_html = malloc(new_size + 1);
	if (escaped_html == NULL)
		event_err(1, "%s: malloc(%d)", __func__, new_size + 1);
	for (i = 0; i < old_size; ++i) {
		const char *replaced = html_replace(html[i]);
		/* this is length checked */
		strcpy(p, replaced);
		p += strlen(replaced);
	}

	*p = '\0';

	return (escaped_html);
}

const char *
evhttp_method(enum evhttp_cmd_type type)
{
	const char *method;

	switch (type) {
	case EVHTTP_REQ_GET:
		method = "GET";
		break;
	case EVHTTP_REQ_POST:
		method = "POST";
		break;
	case EVHTTP_REQ_HEAD:
		method = "HEAD";
		break;
	default:
		method = NULL;
		break;
	}

	return (method);
}

static void
evhttp_add_event(struct event *ev, int timeout, int default_timeout)
{
	if (timeout != 0) {
		struct timeval tv;
		
		timerclear(&tv);
		tv.tv_sec = timeout != -1 ? timeout : default_timeout;
		event_add(ev, &tv);
	} else {
		event_add(ev, NULL);
	}
}

void
evhttp_write_buffer(struct evhttp_connection *evcon,
    void (*cb)(struct evhttp_connection *, void *), void *arg)
{
	event_debug(("%s: preparing to write buffer\n", __func__));

	/* Set call back */
	evcon->cb = cb;
	evcon->cb_arg = arg;

	/* check if the event is already pending */
	if (event_pending(&evcon->ev, EV_WRITE|EV_TIMEOUT, NULL))
		event_del(&evcon->ev);

	event_set(&evcon->ev, evcon->fd, EV_WRITE, evhttp_write, evcon);
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_WRITE_TIMEOUT);
}

/*
 * Create the headers need for an HTTP reply
 */
static void
evhttp_make_header_request(struct evhttp_connection *evcon,
    struct evhttp_request *req)
{
	static char line[1024];
	const char *method;
	
	evhttp_remove_header(req->output_headers, "Accept-Encoding");
	evhttp_remove_header(req->output_headers, "Proxy-Connection");

	/* Generate request line */
	method = evhttp_method(req->type);
	snprintf(line, sizeof(line), "%s %s HTTP/%d.%d\r\n",
	    method, req->uri, req->major, req->minor);
	evbuffer_add(evcon->output_buffer, line, strlen(line));

	/* Add the content length on a post request if missing */
	if (req->type == EVHTTP_REQ_POST &&
	    evhttp_find_header(req->output_headers, "Content-Length") == NULL){
		char size[12];
		snprintf(size, sizeof(size), "%ld",
			 (long)EVBUFFER_LENGTH(req->output_buffer));
		evhttp_add_header(req->output_headers, "Content-Length", size);
	}
}

static int
evhttp_is_connection_close(int flags, struct evkeyvalq* headers)
{
	if (flags & EVHTTP_PROXY_REQUEST) {
		/* proxy connection */
		const char *connection = evhttp_find_header(headers, "Proxy-Connection");
		return (connection == NULL || strcasecmp(connection, "keep-alive") != 0);
	} else {
		const char *connection = evhttp_find_header(headers, "Connection");
		return (connection != NULL && strcasecmp(connection, "close") == 0);
	}
}

static int
evhttp_is_connection_keepalive(struct evkeyvalq* headers)
{
	const char *connection = evhttp_find_header(headers, "Connection");
	return (connection != NULL 
	    && strncasecmp(connection, "keep-alive", 10) == 0);
}

/*
 * Create the headers needed for an HTTP reply
 */
static void
evhttp_make_header_response(struct evhttp_connection *evcon,
    struct evhttp_request *req)
{
	static char line[1024];
	snprintf(line, sizeof(line), "HTTP/%d.%d %d %s\r\n",
	    req->major, req->minor, req->response_code,
	    req->response_code_line);
	evbuffer_add(evcon->output_buffer, line, strlen(line));

	/* Potentially add headers for unidentified content. */
	if (EVBUFFER_LENGTH(req->output_buffer)) {
		if (evhttp_find_header(req->output_headers,
							   "Date") == NULL) {
			char date[50];
			struct tm cur, *cur_p;
			time_t t = time(NULL);
#ifdef WIN32
			cur_p = gmtime(&t);
#else
			gmtime_r(&t, &cur);
			cur_p = &cur;
#endif
			if (strftime(date, sizeof(date),
						 "%a, %d %b %Y %H:%M:%S GMT", cur_p) != 0) {
				evhttp_add_header(req->output_headers, "Date", date);
			}
		}

		if (evhttp_find_header(req->output_headers,
			"Content-Type") == NULL) {
			evhttp_add_header(req->output_headers,
			    "Content-Type", "text/html; charset=ISO-8859-1");
		}

		/* 
		 * we need to add the content length if the user did
		 * not give it, this is required for persistent
		 * connections to work.
		 */
		if (evhttp_find_header(req->output_headers,
			"Transfer-Encoding") == NULL &&
		    evhttp_find_header(req->output_headers,
			"Content-Length") == NULL) {
			static char len[12];
			snprintf(len, sizeof(len), "%ld",
			    (long)EVBUFFER_LENGTH(req->output_buffer));
			evhttp_add_header(req->output_headers,
			    "Content-Length", len);
		}
	}

	/* if the request asked for a close, we send a close, too */
	if (evhttp_is_connection_close(req->flags, req->input_headers)) {
		evhttp_remove_header(req->output_headers, "Connection");
		if (!(req->flags & EVHTTP_PROXY_REQUEST))
		    evhttp_add_header(req->output_headers, "Connection", "close");
		evhttp_remove_header(req->output_headers, "Proxy-Connection");
	}
}

void
evhttp_make_header(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	static char line[1024];
	struct evkeyval *header;

	/*
	 * Depending if this is a HTTP request or response, we might need to
	 * add some new headers or remove existing headers.
	 */
	if (req->kind == EVHTTP_REQUEST) {
		evhttp_make_header_request(evcon, req);
	} else {
		evhttp_make_header_response(evcon, req);
	}

	TAILQ_FOREACH(header, req->output_headers, next) {
		snprintf(line, sizeof(line), "%s: %s\r\n",
		    header->key, header->value);
		evbuffer_add(evcon->output_buffer, line, strlen(line));
	}
	evbuffer_add(evcon->output_buffer, "\r\n", 2);

	if (EVBUFFER_LENGTH(req->output_buffer) >= 0) {
		/*
		 * For a request, we add the POST data, for a reply, this
		 * is the regular data.
		 */
		evbuffer_add_buffer(evcon->output_buffer, req->output_buffer);
	}
}

/* Separated host, port and file from URI */

int
evhttp_hostportfile(char *url, char **phost, u_short *pport, char **pfile)
{
	static char host[1024];
	static char file[1024];
	char *p, *p2;
	int len;
	u_short port;

	len = strlen(HTTP_PREFIX);
	if (strncasecmp(url, HTTP_PREFIX, len))
		return (-1);

	url += len;

	/* We might overrun */
	if (strlcpy(host, url, sizeof (host)) >= sizeof(host))
		return (-1);

	p = strchr(host, '/');
	if (p != NULL) {
		*p = '\0';
		p2 = p + 1;
	} else
		p2 = NULL;

	if (pfile != NULL) {
		/* Generate request file */
		if (p2 == NULL)
			p2 = "";
		snprintf(file, sizeof(file), "/%s", p2);
	}

	p = strchr(host, ':');
	if (p != NULL) {
		*p = '\0';
		port = atoi(p + 1);

		if (port == 0)
			return (-1);
	} else
		port = HTTP_DEFAULTPORT;

	if (phost != NULL)
		*phost = host;
	if (pport != NULL)
		*pport = port;
	if (pfile != NULL)
		*pfile = file;

	return (0);
}

static int
evhttp_connection_incoming_fail(struct evhttp_request *req,
    enum evhttp_connection_error error)
{
	switch (error) {
	case EVCON_HTTP_TIMEOUT:
	case EVCON_HTTP_EOF:
		/* 
		 * these are cases in which we probably should just
		 * close the connection and not send a reply.  this
		 * case may happen when a browser keeps a persistent
		 * connection open and we timeout on the read.
		 */
		return (-1);
	case EVCON_HTTP_INVALID_HEADER:
	default:	/* xxx: probably should just error on default */
		/* the callback looks at the uri to determine errors */
		if (req->uri) {
			free(req->uri);
			req->uri = NULL;
		}

		/* 
		 * the callback needs to send a reply, once the reply has
		 * been send, the connection should get freed.
		 */
		(*req->cb)(req, req->cb_arg);
	}
	
	return (0);
}

void
evhttp_connection_fail(struct evhttp_connection *evcon,
    enum evhttp_connection_error error)
{
	struct evhttp_request* req = TAILQ_FIRST(&evcon->requests);
	void (*cb)(struct evhttp_request *, void *);
	void *cb_arg;
	assert(req != NULL);
	
	if (evcon->flags & EVHTTP_CON_INCOMING) {
		/* 
		 * for incoming requests, there are two different
		 * failure cases.  it's either a network level error
		 * or an http layer error. for problems on the network
		 * layer like timeouts we just drop the connections.
		 * For HTTP problems, we might have to send back a
		 * reply before the connection can be freed.
		 */
		if (evhttp_connection_incoming_fail(req, error) == -1)
			evhttp_connection_free(evcon);
		return;
	}

	/* save the callback for later; the cb might free our object */
	cb = req->cb;
	cb_arg = req->cb_arg;

	TAILQ_REMOVE(&evcon->requests, req, next);
	evhttp_request_free(req);

	/* xxx: maybe we should fail all requests??? */

	/* reset the connection */
	evhttp_connection_reset(evcon);
	
	/* We are trying the next request that was queued on us */
	if (TAILQ_FIRST(&evcon->requests) != NULL)
		evhttp_connection_connect(evcon);

	/* inform the user */
	if (cb != NULL)
		(*cb)(NULL, cb_arg);
}

void
evhttp_write(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	int n;

	if (what == EV_TIMEOUT) {
		evhttp_connection_fail(evcon, EVCON_HTTP_TIMEOUT);
		return;
	}

	n = evbuffer_write(evcon->output_buffer, fd);
	if (n == -1) {
		event_debug(("%s: evbuffer_write", __func__));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	}

	if (n == 0) {
		event_debug(("%s: write nothing", __func__));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	}

	if (EVBUFFER_LENGTH(evcon->output_buffer) != 0) {
		evhttp_add_event(&evcon->ev, 
		    evcon->timeout, HTTP_WRITE_TIMEOUT);
		return;
	}

	/* Activate our call back */
	if (evcon->cb != NULL)
		(*evcon->cb)(evcon, evcon->cb_arg);
}

void
evhttp_connection_done(struct evhttp_connection *evcon)
{
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	int con_outgoing = evcon->flags & EVHTTP_CON_OUTGOING;
    
	/*
	 * if this is an incoming connection, we need to leave the request
	 * on the connection, so that we can reply to it.
	 */
	if (con_outgoing) {
	        int need_close;
		TAILQ_REMOVE(&evcon->requests, req, next);
		req->evcon = NULL;

		need_close = 
		    evhttp_is_connection_close(req->flags, req->input_headers) ||
		    evhttp_is_connection_close(req->flags, req->output_headers);

		/* check if we got asked to close the connection */
		if (need_close)
			evhttp_connection_reset(evcon);

		if (TAILQ_FIRST(&evcon->requests) != NULL) {
			/*
			 * We have more requests; reset the connection
			 * and deal with the next request.  xxx: no
			 * persistent connection right now
			 */
			if (evcon->state != EVCON_CONNECTED)
				evhttp_connection_connect(evcon);
			else
				evhttp_request_dispatch(evcon);
		} else if (!need_close) {
			/*
			 * The connection is going to be persistent, but we
			 * need to detect if the other side closes it.
			 */
			evhttp_connection_start_detectclose(evcon);
		}
	}

	/* notify the user of the request */
	(*req->cb)(req, req->cb_arg);

	/* if this was an outgoing request, we own and it's done. so free it */
	if (con_outgoing) {
		evhttp_request_free(req);
	}
}

/*
 * Handles reading from a chunked request.
 * return 1: all data has been read
 * return 0: more data is expected
 * return -1: data is corrupted
 */

static int
evhttp_handle_chunked_read(struct evhttp_request *req, struct evbuffer *buf)
{
	int len;

	while ((len = EVBUFFER_LENGTH(buf)) > 0) {
		if (req->ntoread < 0) {
			/* Read chunk size */
			char *p = evbuffer_readline(buf);
			char *endp;
			int error;
			if (p == NULL)
				break;
			/* the last chunk is on a new line? */
			if (strlen(p) == 0) {
				free(p);
				continue;
			}
			req->ntoread = strtol(p, &endp, 16);
			error = *p == '\0' || (*endp != '\0' && *endp != ' ');
			free(p);
			if (error) {
				/* could not get chunk size */
				return (-1);
			}
			if (req->ntoread == 0) {
				/* Last chunk */
				return (1);
			}
			continue;
		}

		/* don't have enough to complete a chunk; wait for more */
		if (len < req->ntoread)
			return (0);

		/* Completed chunk */
		evbuffer_add(req->input_buffer,
		    EVBUFFER_DATA(buf), req->ntoread);
		evbuffer_drain(buf, req->ntoread);
		req->ntoread = -1;
		if (req->chunk_cb != NULL) {
			(*req->chunk_cb)(req, req->cb_arg);
			evbuffer_drain(req->input_buffer,
			    EVBUFFER_LENGTH(req->input_buffer));
		}
	}

	return (0);
}

void
evhttp_read_body(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	struct evbuffer *buf = evcon->input_buffer;
	
	if (req->chunked) {
		int res = evhttp_handle_chunked_read(req, buf);
		if (res == 1) {
			/* finished last chunk */
			evhttp_connection_done(evcon);
			return;
		} else if (res == -1) {
			/* corrupted data */
			evhttp_connection_fail(evcon,
			    EVCON_HTTP_INVALID_HEADER);
			return;
		}
	} else if (req->ntoread < 0) {
		/* Read until connection close. */
		evbuffer_add_buffer(req->input_buffer, buf);
	} else if (EVBUFFER_LENGTH(buf) >= req->ntoread) {
		/* Completed content length */
		evbuffer_add(req->input_buffer, EVBUFFER_DATA(buf),
		    req->ntoread);
		evbuffer_drain(buf, req->ntoread);
		req->ntoread = 0;
		evhttp_connection_done(evcon);
		return;
	}
	/* Read more! */
	event_set(&evcon->ev, evcon->fd, EV_READ, evhttp_read, evcon);
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_READ_TIMEOUT);
}

/*
 * Reads data into a buffer structure until no more data
 * can be read on the file descriptor or we have read all
 * the data that we wanted to read.
 * Execute callback when done.
 */

void
evhttp_read(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	struct evbuffer *buf = evcon->input_buffer;
	int n, len;

	if (what == EV_TIMEOUT) {
		evhttp_connection_fail(evcon, EVCON_HTTP_TIMEOUT);
		return;
	}
	n = evbuffer_read(buf, fd, -1);
	len = EVBUFFER_LENGTH(buf);
	event_debug(("%s: got %d on %d\n", __func__, n, fd));
	
	if (n == -1) {
		event_debug(("%s: evbuffer_read", __func__));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	} else if (n == 0) {
		/* Connection closed */
		evhttp_connection_done(evcon);
		return;
	}
	evhttp_read_body(evcon, req);
}

void
evhttp_write_connectioncb(struct evhttp_connection *evcon, void *arg)
{
	/* This is after writing the request to the server */
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	assert(req != NULL);

	/* We are done writing our header and are now expecting the response */
	req->kind = EVHTTP_RESPONSE;

	evhttp_start_read(evcon);
}

/*
 * Clean up a connection object
 */

void
evhttp_connection_free(struct evhttp_connection *evcon)
{
	struct evhttp_request *req;

	/* notify interested parties that this connection is going down */
	if (evcon->fd != -1) {
		if (evcon->state == EVCON_CONNECTED && evcon->closecb != NULL)
			(*evcon->closecb)(evcon, evcon->closecb_arg);
	}

	/* remove all requests that might be queued on this connection */
	while ((req = TAILQ_FIRST(&evcon->requests)) != NULL) {
		TAILQ_REMOVE(&evcon->requests, req, next);
		evhttp_request_free(req);
	}

	if (evcon->http_server != NULL) {
		struct evhttp *http = evcon->http_server;
		TAILQ_REMOVE(&http->connections, evcon, next);
	}

	if (event_initialized(&evcon->close_ev))
		event_del(&evcon->close_ev);

	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);
	
	if (evcon->fd != -1)
		close(evcon->fd);

	if (evcon->address != NULL)
		free(evcon->address);

	if (evcon->input_buffer != NULL)
		evbuffer_free(evcon->input_buffer);

	if (evcon->output_buffer != NULL)
		evbuffer_free(evcon->output_buffer);

	free(evcon);
}

static void
evhttp_request_dispatch(struct evhttp_connection* evcon)
{
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	
	/* this should not usually happy but it's possible */
	if (req == NULL)
		return;

	/* delete possible close detection events */
	evhttp_connection_stop_detectclose(evcon);
	
	/* we assume that the connection is connected already */
	assert(evcon->state == EVCON_CONNECTED);

	/* Create the header from the store arguments */
	evhttp_make_header(evcon, req);

	evhttp_write_buffer(evcon, evhttp_write_connectioncb, NULL);
}

/* Reset our connection state */
void
evhttp_connection_reset(struct evhttp_connection *evcon)
{
	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);

	if (evcon->fd != -1) {
		/* inform interested parties about connection close */
		if (evcon->state == EVCON_CONNECTED && evcon->closecb != NULL)
			(*evcon->closecb)(evcon, evcon->closecb_arg);

		close(evcon->fd);
		evcon->fd = -1;
	}
	evcon->state = EVCON_DISCONNECTED;

	/* remove unneeded flags */
	evcon->flags &= ~EVHTTP_CON_CLOSEDETECT;
}

static void
evhttp_detect_close_cb(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	evhttp_connection_reset(evcon);
}

static void
evhttp_connection_start_detectclose(struct evhttp_connection *evcon)
{
	evcon->flags |= EVHTTP_CON_CLOSEDETECT;

	if (event_initialized(&evcon->close_ev))
		event_del(&evcon->close_ev);
	event_set(&evcon->close_ev, evcon->fd, EV_READ,
	    evhttp_detect_close_cb, evcon);
	event_add(&evcon->close_ev, NULL);
}

static void
evhttp_connection_stop_detectclose(struct evhttp_connection *evcon)
{
	evcon->flags &= ~EVHTTP_CON_CLOSEDETECT;
	event_del(&evcon->close_ev);
}

static void
evhttp_connection_retry(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;

	evcon->state = EVCON_DISCONNECTED;
	evhttp_connection_connect(evcon);
}

/*
 * Call back for asynchronous connection attempt.
 */

static void
evhttp_connectioncb(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	int error;
	socklen_t errsz = sizeof(error);
		
	if (what == EV_TIMEOUT) {
		event_debug(("%s: connection timeout for \"%s:%d\" on %d",
			__func__, evcon->address, evcon->port, evcon->fd));
		goto cleanup;
	}

	/* Check if the connection completed */
	if (getsockopt(evcon->fd, SOL_SOCKET, SO_ERROR, (void*)&error,
		       &errsz) == -1) {
		event_debug(("%s: getsockopt for \"%s:%d\" on %d",
			__func__, evcon->address, evcon->port, evcon->fd));
		goto cleanup;
	}

	if (error) {
		event_debug(("%s: connect failed for \"%s:%d\" on %d: %s",
		    __func__, evcon->address, evcon->port, evcon->fd,
			strerror(error)));
		goto cleanup;
	}

	/* We are connected to the server now */
	event_debug(("%s: connected to \"%s:%d\" on %d\n",
			__func__, evcon->address, evcon->port, evcon->fd));

	/* Reset the retry count as we were successful in connecting */
	evcon->retry_cnt = 0;
	evcon->state = EVCON_CONNECTED;

	/* try to start requests that have queued up on this connection */
	evhttp_request_dispatch(evcon);
	return;

 cleanup:
	if (evcon->retry_max < 0 || evcon->retry_cnt < evcon->retry_max) {
		evtimer_set(&evcon->ev, evhttp_connection_retry, evcon);
		evhttp_add_event(&evcon->ev, MIN(3600, 2 << evcon->retry_cnt),
		    HTTP_CONNECT_TIMEOUT);
		evcon->retry_cnt++;
		return;
	}
	evhttp_connection_reset(evcon);

	/* for now, we just signal all requests by executing their callbacks */
	while (TAILQ_FIRST(&evcon->requests) != NULL) {
		struct evhttp_request *request = TAILQ_FIRST(&evcon->requests);
		TAILQ_REMOVE(&evcon->requests, request, next);
		request->evcon = NULL;

		/* we might want to set an error here */
		request->cb(request, request->cb_arg);
		evhttp_request_free(request);
	}
}

/*
 * Check if we got a valid response code.
 */

int
evhttp_valid_response_code(int code)
{
	if (code == 0)
		return (0);

	return (1);
}

/* Parses the status line of a web server */

int
evhttp_parse_response_line(struct evhttp_request *req, char *line)
{
	char *protocol;
	char *number;
	char *readable;

	protocol = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	number = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	readable = line;

	if (strcmp(protocol, "HTTP/1.0") == 0) {
		req->major = 1;
		req->minor = 0;
	} else if (strcmp(protocol, "HTTP/1.1") == 0) {
		req->major = 1;
		req->minor = 1;
	} else {
		event_debug(("%s: bad protocol \"%s\"",
			__func__, protocol));
		return (-1);
	}

	req->response_code = atoi(number);
	if (!evhttp_valid_response_code(req->response_code)) {
		event_debug(("%s: bad response code \"%s\"",
			__func__, number));
		return (-1);
	}

	if ((req->response_code_line = strdup(readable)) == NULL)
		event_err(1, "%s: strdup", __func__);

	return (0);
}

/* Parse the first line of a HTTP request */

int
evhttp_parse_request_line(struct evhttp_request *req, char *line)
{
	char *method;
	char *uri;
	char *version;

	/* Parse the request line */
	method = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	uri = strsep(&line, " ");
	if (line == NULL)
		return (-1);
	version = strsep(&line, " ");
	if (line != NULL)
		return (-1);

	/* First line */
	if (strcmp(method, "GET") == 0) {
		req->type = EVHTTP_REQ_GET;
	} else if (strcmp(method, "POST") == 0) {
		req->type = EVHTTP_REQ_POST;
	} else if (strcmp(method, "HEAD") == 0) {
		req->type = EVHTTP_REQ_HEAD;
	} else {
		event_debug(("%s: bad method %s on request %p from %s",
			__func__, method, req, req->remote_host));
		return (-1);
	}

	if (strcmp(version, "HTTP/1.0") == 0) {
		req->major = 1;
		req->minor = 0;
	} else if (strcmp(version, "HTTP/1.1") == 0) {
		req->major = 1;
		req->minor = 1;
	} else {
		event_debug(("%s: bad version %s on request %p from %s",
			__func__, version, req, req->remote_host));
		return (-1);
	}

	if ((req->uri = strdup(uri)) == NULL) {
		event_debug(("%s: evhttp_decode_uri", __func__));
		return (-1);
	}

	/* determine if it's a proxy request */
	if (strlen(req->uri) > 0 && req->uri[0] != '/')
		req->flags |= EVHTTP_PROXY_REQUEST;

	return (0);
}

const char *
evhttp_find_header(const struct evkeyvalq *headers, const char *key)
{
	struct evkeyval *header;

	TAILQ_FOREACH(header, headers, next) {
		if (strcasecmp(header->key, key) == 0)
			return (header->value);
	}

	return (NULL);
}

void
evhttp_clear_headers(struct evkeyvalq *headers)
{
	struct evkeyval *header;

	for (header = TAILQ_FIRST(headers);
	    header != NULL;
	    header = TAILQ_FIRST(headers)) {
		TAILQ_REMOVE(headers, header, next);
		free(header->key);
		free(header->value);
		free(header);
	}
}

/*
 * Returns 0,  if the header was successfully removed.
 * Returns -1, if the header could not be found.
 */

int
evhttp_remove_header(struct evkeyvalq *headers, const char *key)
{
	struct evkeyval *header;

	TAILQ_FOREACH(header, headers, next) {
		if (strcasecmp(header->key, key) == 0)
			break;
	}

	if (header == NULL)
		return (-1);

	/* Free and remove the header that we found */
	TAILQ_REMOVE(headers, header, next);
	free(header->key);
	free(header->value);
	free(header);

	return (0);
}

int
evhttp_add_header(struct evkeyvalq *headers,
    const char *key, const char *value)
{
	struct evkeyval *header = NULL;

	if (strchr(value, '\r') != NULL || strchr(value, '\n') != NULL ||
	    strchr(key, '\r') != NULL || strchr(key, '\n') != NULL) {
		/* drop illegal headers */
		event_debug(("%s: dropping illegal header\n"));
		return (-1);
	}

	header = calloc(1, sizeof(struct evkeyval));
	if (header == NULL) {
		event_warn("%s: calloc", __func__);
		return (-1);
	}
	if ((header->key = strdup(key)) == NULL) {
		free(header);
		event_warn("%s: strdup", __func__);
		return (-1);
	}
	if ((header->value = strdup(value)) == NULL) {
		free(header->key);
		free(header);
		event_warn("%s: strdup", __func__);
		return (-1);
	}

	TAILQ_INSERT_TAIL(headers, header, next);

	return (0);
}

/*
 * Parses header lines from a request or a response into the specified
 * request object given an event buffer.
 *
 * Returns
 *   -1  on error
 *    0  when we need to read more headers
 *    1  when all headers have been read.
 */

int
evhttp_parse_lines(struct evhttp_request *req, struct evbuffer* buffer)
{
	u_char *endp;
	int done = 0;

	struct evkeyvalq* headers = req->input_headers;

	/* Some http servers gratuitiously send extra \r\n's when
	   pipelining.  */
	while (EVBUFFER_LENGTH (buffer) > 2
	       && EVBUFFER_DATA (buffer)[0] == '\r'
	       && EVBUFFER_DATA (buffer)[1] == '\n')
	  {
	    log (BOLD ("Server sent gratuitous \\r\\n!"));
	    evbuffer_drain (buffer, 2);
	  }

	while ((endp = evbuffer_find(buffer, (u_char *)"\r\n", 2)) != NULL) {
		char *skey, *svalue;

		if (strncmp((char *)EVBUFFER_DATA(buffer), "\r\n", 2) == 0) {
			evbuffer_drain(buffer, 2);
			/* Last header - Done */
			done = 1;
			break;
		}

		*endp = '\0';
		endp += 2;

		/* Processing of header lines */
		if (req->got_firstline == 0) {
			switch (req->kind) {
			case EVHTTP_REQUEST:
				if (evhttp_parse_request_line(req,
					(char *)EVBUFFER_DATA(buffer)) == -1)
					return (-1);
				break;
			case EVHTTP_RESPONSE:
				if (evhttp_parse_response_line(req,
					(char *)EVBUFFER_DATA(buffer)) == -1)
					return (-1);
				break;
			default:
				return (-1);
			}
			req->got_firstline = 1;
		} else {
			/* Regular header */
			svalue = (char *)EVBUFFER_DATA(buffer);
			skey = strsep(&svalue, ":");
			if (svalue == NULL)
				return (-1);

			svalue += strspn(svalue, " ");

			if (evhttp_add_header(headers, skey, svalue) == -1)
				return (-1);
		}

		/* Move the uncompleted headers forward */
		evbuffer_drain(buffer, endp - EVBUFFER_DATA(buffer));
	}

	return (done);
}

static int
evhttp_get_body_length(struct evhttp_request *req)
{
	struct evkeyvalq *headers = req->input_headers;
	const char *content_length;
	const char *connection;

	content_length = evhttp_find_header(headers, "Content-Length");
	connection = evhttp_find_header(headers, "Connection");
		
	if (content_length == NULL && connection == NULL)
		req->ntoread = -1;
	else if (content_length == NULL &&
	    strcasecmp(connection, "Close") != 0) {
		/* Bad combination, we don't know when it will end */
		event_warnx("%s: we got no content length, but the "
		    "server wants to keep the connection open: %s.",
		    __func__, connection);
		return (-1);
	} else if (content_length == NULL) {
		req->ntoread = -1;
	} else {
		char *endp;
		req->ntoread = strtol(content_length, &endp, 10);
		if (*content_length == '\0' || *endp != '\0') {
			event_warnx("%s: illegal content length: %s",
			    __func__, content_length);
			return (-1);
		}
	}
		
	event_debug(("%s: bytes to read: %d (in buffer %d)\n",
		__func__, req->ntoread,
		EVBUFFER_LENGTH(req->evcon->input_buffer)));

	return (0);
}

static void
evhttp_get_body(struct evhttp_connection *evcon, struct evhttp_request *req)
{
	const char *xfer_enc;
	
	/* If this is a request without a body, then we are done */
	if (req->kind == EVHTTP_REQUEST && req->type != EVHTTP_REQ_POST) {
		evhttp_connection_done(evcon);
		return;
	}
	xfer_enc = evhttp_find_header(req->input_headers, "Transfer-Encoding");
	if (xfer_enc != NULL && strcasecmp(xfer_enc, "chunked") == 0) {
		req->chunked = 1;
		req->ntoread = -1;
	} else {
		if (evhttp_get_body_length(req) == -1) {
			evhttp_connection_fail(evcon,
			    EVCON_HTTP_INVALID_HEADER);
			return;
		}
	}
	evhttp_read_body(evcon, req);
}

void
evhttp_read_header(int fd, short what, void *arg)
{
	struct evhttp_connection *evcon = arg;
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	int n, res;

	if (what == EV_TIMEOUT) {
		event_debug(("%s: timeout on %d\n", __func__, fd));
		evhttp_connection_fail(evcon, EVCON_HTTP_TIMEOUT);
		return;
	}

	n = evbuffer_read(evcon->input_buffer, fd, -1);
	if (n == 0) {
		event_debug(("%s: no more data on %d", __func__, fd));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	}
	if (n == -1) {
		event_debug(("%s: bad read on %d", __func__, fd));
		evhttp_connection_fail(evcon, EVCON_HTTP_EOF);
		return;
	}

	res = evhttp_parse_lines(req, evcon->input_buffer);
	if (res == -1) {
		/* Error while reading, terminate */
		event_debug(("%s: bad header lines on %d\n", __func__, fd));
		evhttp_connection_fail(evcon, EVCON_HTTP_INVALID_HEADER);
		return;
	} else if (res == 0) {
		/* Need more header lines */
		evhttp_add_event(&evcon->ev, 
		    evcon->timeout, HTTP_READ_TIMEOUT);
		return;
	}

	/* Done reading headers, do the real work */
	switch (req->kind) {
	case EVHTTP_REQUEST:
		event_debug(("%s: checking for post data on %d\n",
				__func__, fd));
		evhttp_get_body(evcon, req);
		break;

	case EVHTTP_RESPONSE:
		if (req->response_code == HTTP_NOCONTENT ||
		    req->response_code == HTTP_NOTMODIFIED ||
		    (req->response_code >= 100 && req->response_code < 200)) {
			event_debug(("%s: skipping body for code %d\n",
					__func__, req->response_code));
			evhttp_connection_done(evcon);
		} else {
			event_debug(("%s: start of read body for %s on %d\n",
				__func__, req->remote_host, fd));
			evhttp_get_body(evcon, req);
		}
		break;

	default:
		event_warnx("%s: bad header on %d", __func__, fd);
		evhttp_connection_fail(evcon, EVCON_HTTP_INVALID_HEADER);
		break;
	}
}

/*
 * Creates a TCP connection to the specified port and executes a callback
 * when finished.  Failure or sucess is indicate by the passed connection
 * object.
 *
 * Although this interface accepts a hostname, it is intended to take
 * only numeric hostnames so that non-blocking DNS resolution can
 * happen elsewhere.
 */

struct evhttp_connection *
evhttp_connection_new(const char *address, unsigned short port)
{
	struct evhttp_connection *evcon = NULL;
	
	event_debug(("Attempting connection to %s:%d\n", address, port));

	if ((evcon = calloc(1, sizeof(struct evhttp_connection))) == NULL) {
		event_warn("%s: calloc failed", __func__);
		goto error;
	}

	evcon->fd = -1;
	evcon->port = port;

	evcon->timeout = -1;
	evcon->retry_cnt = evcon->retry_max = 0;

	if ((evcon->address = strdup(address)) == NULL) {
		event_warn("%s: strdup failed", __func__);
		goto error;
	}

	if ((evcon->input_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new failed", __func__);
		goto error;
	}

	if ((evcon->output_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new failed", __func__);
		goto error;
	}
	
	evcon->state = EVCON_DISCONNECTED;
	TAILQ_INIT(&evcon->requests);

	return (evcon);
	
 error:
	if (evcon != NULL)
		evhttp_connection_free(evcon);
	return (NULL);
}

void
evhttp_connection_set_timeout(struct evhttp_connection *evcon,
    int timeout_in_secs)
{
	evcon->timeout = timeout_in_secs;
}

void
evhttp_connection_set_retries(struct evhttp_connection *evcon,
    int retry_max)
{
	evcon->retry_max = retry_max;
}

void
evhttp_connection_set_closecb(struct evhttp_connection *evcon,
    void (*cb)(struct evhttp_connection *, void *), void *cbarg)
{
	evcon->closecb = cb;
	evcon->closecb_arg = cbarg;
}

void
evhttp_connection_get_peer(struct evhttp_connection *evcon,
    char **address, u_short *port)
{
	*address = evcon->address;
	*port = evcon->port;
}

int
evhttp_connection_connect(struct evhttp_connection *evcon)
{
	if (evcon->state == EVCON_CONNECTING)
		return (0);
	
	evhttp_connection_reset(evcon);

	assert(!(evcon->flags & EVHTTP_CON_INCOMING));
	evcon->flags |= EVHTTP_CON_OUTGOING;
	
	/* Do async connection to HTTP server */
	if ((evcon->fd = make_socket(
		     0, evcon->address, evcon->port)) == -1) {
		event_debug(("%s: failed to connect to \"%s:%d\"",
			__func__, evcon->address, evcon->port));
		return (-1);
	}

	/* Set up a callback for successful connection setup */
	event_set(&evcon->ev, evcon->fd, EV_WRITE, evhttp_connectioncb, evcon);
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_CONNECT_TIMEOUT);

	evcon->state = EVCON_CONNECTING;
	
	return (0);
}

/*
 * Starts an HTTP request on the provided evhttp_connection object.
 * If the connection object is not connected to the web server already,
 * this will start the connection.
 */

int
evhttp_make_request(struct evhttp_connection *evcon,
    struct evhttp_request *req,
    enum evhttp_cmd_type type, const char *uri)
{
	/* We are making a request */
	req->kind = EVHTTP_REQUEST;
	req->type = type;
	if (req->uri != NULL)
		free(req->uri);
	if ((req->uri = strdup(uri)) == NULL)
		event_err(1, "%s: strdup", __func__);

	/* Set the protocol version if it is not supplied */
	if (!req->major && !req->minor) {
		req->major = 1;
		req->minor = 1;
	}
	
	assert(req->evcon == NULL);
	req->evcon = evcon;
	assert(!(req->flags & EVHTTP_REQ_OWN_CONNECTION));
	
	TAILQ_INSERT_TAIL(&evcon->requests, req, next);

	/* If the connection object is not connected; make it so */
	if (evcon->state != EVCON_CONNECTED)
		return (evhttp_connection_connect(evcon));

	/*
	 * If it's connected already and we are the first in the queue,
	 * then we can dispatch this request immediately.  Otherwise, it
	 * will be dispatched once the pending requests are completed.
	 */
	if (TAILQ_FIRST(&evcon->requests) == req)
		evhttp_request_dispatch(evcon);

	return (0);
}

/*
 * Reads data from file descriptor into request structure
 * Request structure needs to be set up correctly.
 */

void
evhttp_start_read(struct evhttp_connection *evcon)
{
	/* Set up an event to read the headers */
	if (event_initialized(&evcon->ev))
		event_del(&evcon->ev);
	event_set(&evcon->ev, evcon->fd, EV_READ, evhttp_read_header, evcon);
	
	evhttp_add_event(&evcon->ev, evcon->timeout, HTTP_READ_TIMEOUT);
}

void
evhttp_send_done(struct evhttp_connection *evcon, void *arg)
{
	int need_close;
	struct evhttp_request *req = TAILQ_FIRST(&evcon->requests);
	TAILQ_REMOVE(&evcon->requests, req, next);

	/* delete possible close detection events */
	evhttp_connection_stop_detectclose(evcon);
	
	need_close =
	    (req->minor == 0 &&
		!evhttp_is_connection_keepalive(req->input_headers))||
	    evhttp_is_connection_close(req->flags, req->input_headers) ||
	    evhttp_is_connection_close(req->flags, req->output_headers);

	assert(req->flags & EVHTTP_REQ_OWN_CONNECTION);
	evhttp_request_free(req);

	if (need_close) {
		evhttp_connection_free(evcon);
		return;
	} 

	/* we have a persistent connection; try to accept another request. */
	if (evhttp_associate_new_request_with_connection(evcon) == -1)
		evhttp_connection_free(evcon);
}

/*
 * Returns an error page.
 */

void
evhttp_send_error(struct evhttp_request *req, int error, const char *reason)
{
	char *fmt = "<HTML><HEAD>\n"
	    "<TITLE>%d %s</TITLE>\n"
	    "</HEAD><BODY>\n"
	    "<H1>Method Not Implemented</H1>\n"
	    "Invalid method in request<P>\n"
	    "</BODY></HTML>\n";

	struct evbuffer *buf = evbuffer_new();

	/* close the connection on error */
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_response_code(req, error, reason);

	evbuffer_add_printf(buf, fmt, error, reason);

	evhttp_send_page(req, buf);

	evbuffer_free(buf);
}

/* Requires that headers and response code are already set up */

static inline void
evhttp_send(struct evhttp_request *req, struct evbuffer *databuf)
{
	struct evhttp_connection *evcon = req->evcon;

	assert(TAILQ_FIRST(&evcon->requests) == req);

	/* xxx: not sure if we really should expost the data buffer this way */
	if (databuf != NULL)
		evbuffer_add_buffer(req->output_buffer, databuf);
	
	/* Adds headers to the response */
	evhttp_make_header(evcon, req);

	evhttp_write_buffer(evcon, evhttp_send_done, NULL);
}

void
evhttp_send_reply(struct evhttp_request *req, int code, const char *reason,
    struct evbuffer *databuf)
{
	/* set up to watch for client close */
	evhttp_connection_start_detectclose(req->evcon);
	evhttp_response_code(req, code, reason);
	
	evhttp_send(req, databuf);
}

void
evhttp_send_reply_start(struct evhttp_request *req, int code,
    const char *reason)
{
	/* set up to watch for client close */
	evhttp_connection_start_detectclose(req->evcon);
	evhttp_response_code(req, code, reason);
	if (req->major == 1 && req->minor == 1) {
		/* use chunked encoding for HTTP/1.1 */
		evhttp_add_header(req->output_headers, "Transfer-Encoding",
		    "chunked");
		req->chunked = 1;
	}
	evhttp_make_header(req->evcon, req);
	evhttp_write_buffer(req->evcon, NULL, NULL);
}

void
evhttp_send_reply_chunk(struct evhttp_request *req, struct evbuffer *databuf)
{
	if (req->chunked) {
		evbuffer_add_printf(req->evcon->output_buffer, "%x\r\n",
		    EVBUFFER_LENGTH(databuf));
	}
	evbuffer_add_buffer(req->evcon->output_buffer, databuf);
	evhttp_write_buffer(req->evcon, NULL, NULL);
}

void
evhttp_send_reply_end(struct evhttp_request *req)
{
	struct evhttp_connection *evcon = req->evcon;

	if (req->chunked) {
		evbuffer_add(req->evcon->output_buffer, "0\r\n\r\n", 5);
		evhttp_write_buffer(req->evcon, evhttp_send_done, NULL);
		req->chunked = 0;
	} else if (!event_pending(&evcon->ev, EV_WRITE|EV_TIMEOUT, NULL)) {
		/* let the connection know that we are done with the request */
		evhttp_send_done(evcon, NULL);
	} else {
		/* make the callback execute after all data has been written */
		evcon->cb = evhttp_send_done;
		evcon->cb_arg = NULL;
	}
}

void
evhttp_response_code(struct evhttp_request *req, int code, const char *reason)
{
	req->kind = EVHTTP_RESPONSE;
	req->response_code = code;
	if (req->response_code_line != NULL)
		free(req->response_code_line);
	req->response_code_line = strdup(reason);
}

void
evhttp_send_page(struct evhttp_request *req, struct evbuffer *databuf)
{
	if (!req->major || !req->minor) {
		req->major = 1;
		req->minor = 1;
	}
	
	if (req->kind != EVHTTP_RESPONSE)
		evhttp_response_code(req, 200, "OK");

	evhttp_clear_headers(req->output_headers);
	evhttp_add_header(req->output_headers, "Content-Type", "text/html");
	evhttp_add_header(req->output_headers, "Connection", "close");

	evhttp_send(req, databuf);
}

static const char uri_chars[256] = {
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 1, 0, 0, 1, 0, 0, 1,   1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 1, 0, 0,
	/* 64 */
	1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 0, 0, 1,
	0, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,   1, 1, 1, 0, 0, 0, 1, 0,
	/* 128 */
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	/* 192 */
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
};

/*
 * Helper functions to encode/decode a URI.
 * The returned string must be freed by the caller.
 */
char *
evhttp_encode_uri(const char *uri)
{
	struct evbuffer *buf = evbuffer_new();
	char *p;

	for (p = (char *)uri; *p != '\0'; p++) {
		if (uri_chars[(u_char)(*p)]) {
			evbuffer_add(buf, p, 1);
		} else {
			evbuffer_add_printf(buf, "%%%02X", (u_char)(*p));
		}
	}
	evbuffer_add(buf, "", 1);
	p = strdup((char *)EVBUFFER_DATA(buf));
	evbuffer_free(buf);
	
	return (p);
}

char *
evhttp_decode_uri(const char *uri)
{
	char c, *ret;
	int i, j, in_query = 0;
	
	ret = malloc(strlen(uri) + 1);
	if (ret == NULL)
		event_err(1, "%s: malloc(%d)", __func__, strlen(uri) + 1);

	for (i = j = 0; uri[i] != '\0'; i++) {
		c = uri[i];
		if (c == '?') {
			in_query = 1;
		} else if (c == '+' && in_query) {
			c = ' ';
		} else if (c == '%' && isxdigit((unsigned char)uri[i+1]) &&
		    isxdigit((unsigned char)uri[i+2])) {
			char tmp[] = { uri[i+1], uri[i+2], '\0' };
			c = (char)strtol(tmp, NULL, 16);
			i += 2;
		}
		ret[j++] = c;
	}
	ret[j] = '\0';
	
	return (ret);
}

/* 
 * Helper function to parse out arguments in a query.
 * The arguments are separated by key and value.
 * URI should already be decoded.
 */

void
evhttp_parse_query(const char *uri, struct evkeyvalq *headers)
{
	char *line;
	char *argument;
	char *p;

	TAILQ_INIT(headers);

	/* No arguments - we are done */
	if (strchr(uri, '?') == NULL)
		return;

	if ((line = strdup(uri)) == NULL)
		event_err(1, "%s: strdup", __func__);


	argument = line;

	/* We already know that there has to be a ? */
	strsep(&argument, "?");

	p = argument;
	while (p != NULL && *p != '\0') {
		char *key, *value;
		argument = strsep(&p, "&");

		value = argument;
		key = strsep(&value, "=");
		if (value == NULL)
			goto error;

		value = evhttp_decode_uri(value);
		event_debug(("Query Param: %s -> %s\n", key, value));
		evhttp_add_header(headers, key, value);
		free(value);
	}

 error:
	free(line);
}

static struct evhttp_cb *
evhttp_dispatch_callback(struct httpcbq *callbacks, struct evhttp_request *req)
{
	struct evhttp_cb *cb;
	size_t offset = 0;

	/* Test for different URLs */
	char *p = strchr(req->uri, '?');
	if (p != NULL)
		offset = (size_t)(p - req->uri);

	TAILQ_FOREACH(cb, callbacks, next) {
		int res = 0;
		if (p == NULL) {
			res = strcmp(cb->what, req->uri) == 0;
		} else {
			res = ((strncmp(cb->what, req->uri, offset) == 0) &&
					(cb->what[offset] == '\0'));
		}

		if (res)
			return (cb);
	}

	return (NULL);
}

void
evhttp_handle_request(struct evhttp_request *req, void *arg)
{
	struct evhttp *http = arg;
	struct evhttp_cb *cb = NULL;

	if (req->uri == NULL) {
		evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
		return;
	}

	if ((cb = evhttp_dispatch_callback(&http->callbacks, req)) != NULL) {
		(*cb->cb)(req, cb->cbarg);
		return;
	}

	/* Generic call back */
	if (http->gencb) {
		(*http->gencb)(req, http->gencbarg);
		return;
	} else {
		/* We need to send a 404 here */
		char *fmt = "<html><head>"
		    "<title>404 Not Found</title>"
		    "</head><body>"
		    "<h1>Not Found</h1>"
		    "<p>The requested URL %s was not found on this server.</p>"
		    "</body></html>\n";

		char *escaped_html = evhttp_htmlescape(req->uri);
		struct evbuffer *buf = evbuffer_new();

		evhttp_response_code(req, HTTP_NOTFOUND, "Not Found");

		evbuffer_add_printf(buf, fmt, escaped_html);

		free(escaped_html);
		
		evhttp_send_page(req, buf);

		evbuffer_free(buf);
	}
}

static void
accept_socket(int fd, short what, void *arg)
{
	struct evhttp *http = arg;
	struct sockaddr_storage ss;
	socklen_t addrlen = sizeof(ss);
	int nfd;

	if ((nfd = accept(fd, (struct sockaddr *)&ss, &addrlen)) == -1) {
		event_warn("%s: bad accept", __func__);
		return;
	}
        if (event_make_socket_nonblocking(nfd) < 0)
                return;

	evhttp_get_request(http, nfd, (struct sockaddr *)&ss, addrlen);
}

static int
bind_socket(struct evhttp *http, const char *address, u_short port)
{
	struct event *ev = &http->bind_ev;
	int fd;

	if ((fd = make_socket(1, address, port)) == -1)
		return (-1);

	if (listen(fd, 10) == -1) {
		event_warn("%s: listen", __func__);
		return (-1);
	}

	/* Schedule the socket for accepting */
	event_set(ev, fd, EV_READ | EV_PERSIST, accept_socket, http);
	event_add(ev, NULL);

	event_debug(("Bound to port %d - Awaiting connections ... ", port));

	return (0);
}

/*
 * Start a web server on the specified address and port.
 */

struct evhttp *
evhttp_start(const char *address, u_short port)
{
	struct evhttp *http;

	if ((http = calloc(1, sizeof(struct evhttp))) == NULL) {
		event_warn("%s: calloc", __func__);
		return (NULL);
	}

	http->timeout = -1;

	TAILQ_INIT(&http->callbacks);
	TAILQ_INIT(&http->connections);

	if (bind_socket(http, address, port) == -1) {
		free(http);
		return (NULL);
	}

	return (http);
}

void
evhttp_free(struct evhttp* http)
{
	struct evhttp_cb *http_cb;
	struct evhttp_connection *evcon;
	int fd = http->bind_ev.ev_fd;

	/* Remove the accepting part */
	event_del(&http->bind_ev);
	close(fd);

	while ((evcon = TAILQ_FIRST(&http->connections)) != NULL) {
		/* evhttp_connection_free removes the connection */
		evhttp_connection_free(evcon);
	}

	while ((http_cb = TAILQ_FIRST(&http->callbacks)) != NULL) {
		TAILQ_REMOVE(&http->callbacks, http_cb, next);
		free(http_cb->what);
		free(http_cb);
	}
	
	free(http);
}

void
evhttp_set_timeout(struct evhttp* http, int timeout_in_secs)
{
	http->timeout = timeout_in_secs;
}

void
evhttp_set_cb(struct evhttp *http, const char *uri,
    void (*cb)(struct evhttp_request *, void *), void *cbarg)
{
	struct evhttp_cb *http_cb;

	if ((http_cb = calloc(1, sizeof(struct evhttp_cb))) == NULL)
		event_err(1, "%s: calloc", __func__);

	http_cb->what = strdup(uri);
	http_cb->cb = cb;
	http_cb->cbarg = cbarg;

	TAILQ_INSERT_TAIL(&http->callbacks, http_cb, next);
}

int
evhttp_del_cb(struct evhttp *http, const char *uri)
{
	struct evhttp_cb *http_cb;

	TAILQ_FOREACH(http_cb, &http->callbacks, next) {
		if (strcmp(http_cb->what, uri) == 0)
			break;
	}
	if (http_cb == NULL)
		return (-1);

	TAILQ_REMOVE(&http->callbacks, http_cb, next);
	free(http_cb->what);
	free(http_cb);

	return (0);
}

void
evhttp_set_gencb(struct evhttp *http,
    void (*cb)(struct evhttp_request *, void *), void *cbarg)
{
	http->gencb = cb;
	http->gencbarg = cbarg;
}

/*
 * Request related functions
 */

struct evhttp_request *
evhttp_request_new(void (*cb)(struct evhttp_request *, void *), void *arg)
{
	struct evhttp_request *req = NULL;

	/* Allocate request structure */
	if ((req = calloc(1, sizeof(struct evhttp_request))) == NULL) {
		event_warn("%s: calloc", __func__);
		goto error;
	}

	req->kind = EVHTTP_RESPONSE;
	req->input_headers = calloc(1, sizeof(struct evkeyvalq));
	if (req->input_headers == NULL) {
		event_warn("%s: calloc", __func__);
		goto error;
	}
	TAILQ_INIT(req->input_headers);

	req->output_headers = calloc(1, sizeof(struct evkeyvalq));
	if (req->output_headers == NULL) {
		event_warn("%s: calloc", __func__);
		goto error;
	}
	TAILQ_INIT(req->output_headers);

	if ((req->input_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new", __func__);
		goto error;
	}

	if ((req->output_buffer = evbuffer_new()) == NULL) {
		event_warn("%s: evbuffer_new", __func__);
		goto error;
	}

	req->cb = cb;
	req->cb_arg = arg;

	return (req);

 error:
	if (req != NULL)
		evhttp_request_free(req);
	return (NULL);
}

void
evhttp_request_free(struct evhttp_request *req)
{
	if (req->remote_host != NULL)
		free(req->remote_host);
	if (req->uri != NULL)
		free(req->uri);
	if (req->response_code_line != NULL)
		free(req->response_code_line);

	evhttp_clear_headers(req->input_headers);
	free(req->input_headers);

	evhttp_clear_headers(req->output_headers);
	free(req->output_headers);

	if (req->input_buffer != NULL)
		evbuffer_free(req->input_buffer);

	if (req->output_buffer != NULL)
		evbuffer_free(req->output_buffer);

	free(req);
}

void
evhttp_request_set_chunked_cb(struct evhttp_request *req,
    void (*cb)(struct evhttp_request *, void *))
{
	req->chunk_cb = cb;
}

/*
 * Allows for inspection of the request URI
 */

const char *
evhttp_request_uri(struct evhttp_request *req) {
	if (req->uri == NULL)
		event_debug(("%s: request %p has no uri\n", req));
	return (req->uri);
}

/*
 * Takes a file descriptor to read a request from.
 * The callback is executed once the whole request has been read.
 */

static struct evhttp_connection*
evhttp_get_request_connection(
	int fd, struct sockaddr *sa, socklen_t salen)
{
	struct evhttp_connection *evcon;
	char *hostname, *portname;

	name_from_addr(sa, salen, &hostname, &portname);
	event_debug(("%s: new request from %s:%s on %d\n",
			__func__, hostname, portname, fd));

	/* we need a connection object to put the http request on */
	if ((evcon = evhttp_connection_new(hostname, atoi(portname))) == NULL)
		return (NULL);
	evcon->flags |= EVHTTP_CON_INCOMING;
	evcon->state = EVCON_CONNECTED;
	
	evcon->fd = fd;

	return (evcon);
}

static int
evhttp_associate_new_request_with_connection(struct evhttp_connection *evcon)
{
	struct evhttp *http = evcon->http_server;
	struct evhttp_request *req;
	if ((req = evhttp_request_new(evhttp_handle_request, http)) == NULL)
		return (-1);

	req->evcon = evcon;	/* the request ends up owning the connection */
	req->flags |= EVHTTP_REQ_OWN_CONNECTION;
	
	TAILQ_INSERT_TAIL(&evcon->requests, req, next);
	
	req->kind = EVHTTP_REQUEST;
	
	if ((req->remote_host = strdup(evcon->address)) == NULL)
		event_err(1, "%s: strdup", __func__);
	req->remote_port = evcon->port;

	evhttp_start_read(evcon);
	
	return (0);
}

void
evhttp_get_request(struct evhttp *http, int fd,
    struct sockaddr *sa, socklen_t salen)
{
	struct evhttp_connection *evcon;

	evcon = evhttp_get_request_connection(fd, sa, salen);
	if (evcon == NULL)
		return;

	/* the timeout can be used by the server to close idle connections */
	if (http->timeout != -1)
		evhttp_connection_set_timeout(evcon, http->timeout);

	/* 
	 * if we want to accept more than one request on a connection,
	 * we need to know which http server it belongs to.
	 */
	evcon->http_server = http;
	TAILQ_INSERT_TAIL(&http->connections, evcon, next);
	
	if (evhttp_associate_new_request_with_connection(evcon) == -1)
		evhttp_connection_free(evcon);
}


/*
 * Network helper functions that we do not want to export to the rest of
 * the world.
 */
#if 0 /* Unused */
static struct addrinfo *
addr_from_name(char *address)
{
#ifdef HAVE_GETADDRINFO
        struct addrinfo ai, *aitop;
        int ai_result;

        memset(&ai, 0, sizeof (ai));
        ai.ai_family = AF_INET;
        ai.ai_socktype = SOCK_RAW;
        ai.ai_flags = 0;
        if ((ai_result = getaddrinfo(address, NULL, &ai, &aitop)) != 0) {
                if ( ai_result == EAI_SYSTEM )
                        event_warn("getaddrinfo");
                else
                        event_warnx("getaddrinfo: %s", gai_strerror(ai_result));
        }

	return (aitop);
#else
	assert(0);
	return NULL; // XXXXX Use gethostbyname, if this function is ever used.
#endif
}
#endif

static void
name_from_addr(struct sockaddr *sa, socklen_t salen,
    char **phost, char **pport)
{
#ifdef HAVE_GETNAMEINFO
	static char ntop[NI_MAXHOST];
	static char strport[NI_MAXSERV];
	int ni_result;

	if ((ni_result = getnameinfo(sa, salen,
		ntop, sizeof(ntop), strport, sizeof(strport),
		NI_NUMERICHOST|NI_NUMERICSERV)) != 0) {
		if (ni_result == EAI_SYSTEM)
			event_err(1, "getnameinfo failed");
		else
			event_errx(1, "getnameinfo failed: %s", gai_strerror(ni_result));
	}
 
	*phost = ntop;
	*pport = strport;
#else
	// XXXX
#endif
}

/* Either connect or bind */

static int
make_socket_ai(int should_bind, struct addrinfo *ai)
{
        struct linger linger;
        int fd, on = 1, r;
	int serrno;

        /* Create listen socket */
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
                event_warn("socket");
                return (-1);
        }

        if (event_make_socket_nonblocking(fd) < 0)
                goto out;

#ifndef WIN32
        if (fcntl(fd, F_SETFD, 1) == -1) {
                event_warn("fcntl(F_SETFD)");
                goto out;
        }
#endif

        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on));
        linger.l_onoff = 1;
        linger.l_linger = 5;
        setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&linger, sizeof(linger));

	if (should_bind)
		r = bind(fd, ai->ai_addr, ai->ai_addrlen);
	else
		r = connect(fd, ai->ai_addr, ai->ai_addrlen);
	if (r == -1) {
#ifdef WIN32
		int tmp_error = WSAGetLastError();
		if (tmp_error != WSAEWOULDBLOCK && tmp_error != WSAEINVAL &&
		    tmp_error != WSAEINPROGRESS) {
			goto out;
		}
#else
		if (errno != EINPROGRESS) {
			goto out;
		}
#endif
	}

	return (fd);

 out:
	serrno = errno;
	close(fd);
	errno = serrno;
	return (-1);
}

static int
make_socket(int should_bind, const char *address, u_short port)
{
	int fd;
        struct addrinfo ai, *aitop = NULL;
#ifdef HAVE_GETADDRINFO
        char strport[NI_MAXSERV];
        int ai_result;

        memset(&ai, 0, sizeof (ai));
        ai.ai_family = AF_INET;
        ai.ai_socktype = SOCK_STREAM;
        ai.ai_flags = should_bind ? AI_PASSIVE : 0;
        snprintf(strport, sizeof (strport), "%d", port);
        if ((ai_result = getaddrinfo(address, strport, &ai, &aitop)) != 0) {
                if ( ai_result == EAI_SYSTEM )
                        event_warn("getaddrinfo");
                else
                        event_warnx("getaddrinfo: %s", gai_strerror(ai_result));
		return (-1);
        }
#else
	if (fake_getaddrinfo(address, &ai) < 0) {
		event_warn("fake_getaddrinfo");
		return (-1);
	}
	aitop = &ai;
#endif

	fd = make_socket_ai(should_bind, aitop);

#ifdef HAVE_GETADDRINFO
	freeaddrinfo(aitop);
#else
	fake_freeaddrinfo(aitop);
#endif

	return (fd);
}