/* gzip.c
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
#include <zlib.h>
#include <assert.h>

#include "log.h"

#define MAX(a, b) ((a) < (b) ? (b) : (a))

struct evbuffer *
evbuffer_gzip (struct evbuffer *source, int min_ratio, int deflate_flag)
{
  struct evbuffer *target = evbuffer_new ();

  int ret, flush;
  z_stream strm;

  /* allocate deflate state */
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  int window_size = 15 /* default.  */ + 16 /* gzip encoding */;
  if (deflate_flag)
    window_size = -15;

  ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
		     window_size,
		     8 /* default.  */,
		     Z_DEFAULT_STRATEGY /* default.  But Z_RLE is
					   better for PNG data.  */);
  if (ret != Z_OK)
    goto err;

  /* compress until end of file */
  strm.avail_in = EVBUFFER_LENGTH (source);
  strm.next_in = EVBUFFER_DATA (source);
  flush = Z_FINISH;

  /* We keep this relatively small and on the very hot stack, which is
     better than allocating lots of cold memory.  */
  unsigned char buffer[4096 * 4];

  /* run deflate() on input until output buffer not full, finish
     compression if all of source has been read in */
  do
    {
      strm.avail_out = sizeof (buffer);
      strm.next_out = buffer;
      ret = deflate(&strm, flush);    /* no bad return value */
      assert(ret != Z_STREAM_ERROR);  /* state not clobbered */

      int produced = sizeof (buffer) - strm.avail_out;
      int consumed = EVBUFFER_LENGTH (source) - strm.avail_in;
      if (/* If we've consumed less than half of the data and the
	   amount of produced data exceeds the desired threshold,
	   abort.  */
	  (consumed < EVBUFFER_LENGTH (source) / 2
	   && (100 * produced) / consumed > min_ratio)
	  /* If we've consumed more than half but not yet all data and
	     the amount of produced data exceeds 97% of the consumed
	     data, abort.  */
	  || (consumed < produced
	      && (100 * produced) / consumed > MAX (97, min_ratio))
	  /* The amount of data produced is more than 99% of the
	     source.  */
	  || (100 * produced) / consumed > MAX (99, min_ratio))
	{
	  log (BOLD ("Aborted compression: %d/%d: %d%%"),
	       produced, consumed,
	       (100 * produced) / consumed);
	  goto err_with_stream;
	}

      if (evbuffer_add (target, buffer, produced) < 0)
	goto err_with_stream;
    }
  while (strm.avail_out == 0);
  assert(strm.avail_in == 0);     /* all input will be used */
  assert(ret == Z_STREAM_END);        /* stream will be complete */

  /* clean up and return */
  (void)deflateEnd(&strm);

  return target;

 err_with_stream:
  (void)deflateEnd(&strm);
 err:
  evbuffer_free (target);
  return NULL;
}
