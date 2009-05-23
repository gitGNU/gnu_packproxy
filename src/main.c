/* main.c
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
#include <error.h>
#include <errno.h>
#include <sys/socket.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "user_conn.h"
#include "log.h"
#include "opts.h"

/* Event handler for incoming connections.  */
static void
socket_event (int fd, short event, void *arg)
{
  /* Retrieve the new file descriptor.  */
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof (addr);
  int connfd = accept (fd, (struct sockaddr *) &addr, &addr_len);
  if (connfd < 0)
    error (errno, 1, "accept");

  char ip[16] = "unknown";
  if (addr.sin_family == AF_INET)
    {
      inet_ntop (addr.sin_family, &addr.sin_addr, ip, sizeof (ip));
      ip[sizeof (ip) - 1] = 0;
    }

  /* Create an event structure and register it.  */
  struct user_conn *user_conn = user_conn_new (connfd, ip);
  if (! user_conn)
    {
      log ("Failed to allocate connection data structure.");
      close (connfd);
    }
}

struct event_base *event_base;


static int
pack_proxy (struct arguments_t *arguments)
{
  int ret;

  /* We block SIGPIPE.  If a client closes the socket and we write to
     it, the write returns EPIPE and delivers a SIGPIPE.  The default
     action is to abort.  This is not what we want.  Blocking SIGPIPE
     is enough as the right thing will happen: the write will fail and
     we will notice that the socket has been closed.  */
  sigset_t sigset;
  sigemptyset (&sigset);
  sigaddset (&sigset, SIGPIPE);
  ret = sigprocmask (SIG_BLOCK, &sigset, NULL);
  if (ret < 0)
    error (errno, 1, "sigprocmask (SIG_BLOCK, SIGPIPE)");

  event_base = event_init ();
  if (! event_base)
    error (0, 1, "Failed to initialize libevent.");

  /* Bind to the server socket.  */
  int server_socket = socket (AF_INET, SOCK_STREAM, 0);
  if (server_socket == -1)
    error (errno, 1, "socket()");

  /* On close, block for at most one second and then cause a
     connection reset.  */
  struct linger linger;
  linger.l_linger = 0;
  linger.l_onoff = 0;
  ret = setsockopt (server_socket, SOL_SOCKET, SO_LINGER,
		    (void *) &linger, (socklen_t) sizeof (linger));
  if (ret < 0)
    error (errno, 0, "warning: setsockopt (SO_LINGER)");
  int reuse = 1;
  ret = setsockopt (server_socket, SOL_SOCKET, SO_REUSEADDR,
		    (void *) &reuse, (socklen_t) sizeof (reuse));
  if (ret < 0)
    error (errno, 0, "warning: setsockopt (SO_REUSEADDR)");

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons (arguments->ziproxy_ng.port);
  ret = bind (server_socket, (struct sockaddr *) &addr, sizeof (addr));
  if (ret < 0)
    error (errno, 1, "bind()");

  ret = listen (server_socket, 50);
  if (ret < 0)
    error (errno, 1, "listen");

  /* Set up an event source to handle incoming connections.  */
  struct event socket_event_source;
  event_set (&socket_event_source, server_socket, EV_READ|EV_WRITE|EV_PERSIST,
	     socket_event, NULL);
  ret = event_add (&socket_event_source, NULL /* No timeout.  */);
  if (ret < 0)
    error (errno, 1, "event_add");

  /* Event the event loop.  Never returns.  */
  event_dispatch ();

  return 0;
}

int
main (int argc, char *argv[])
{
  struct arguments_t arguments;

  memset (&arguments, 0, sizeof (arguments));

  parse_opts (argc, argv, &arguments);

  return pack_proxy (&arguments);
}
