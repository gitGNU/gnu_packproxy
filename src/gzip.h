#ifndef GZIP_H
#define GZIP_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <zlib.h>

/* gzip SOURCE.  Result is returned or NULL, if an error occured.  */
struct evbuffer *evbuffer_gzip (struct evbuffer *source);

#endif
