#ifndef GZIP_H
#define GZIP_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <zlib.h>

/* gzip SOURCE.  Result is returned or NULL, if an error occured.  If
   the output appears to be larger than MIN_PERCENT of the input, then
   compression is aborted.  */
struct evbuffer *evbuffer_gzip (struct evbuffer *source,
				int min_percent);

#endif
