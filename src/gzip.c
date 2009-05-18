#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <zlib.h>
#include <assert.h>

#include "log.h"

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
      if ((100 * produced) / consumed > min_ratio)
	{
	  log (BOLD ("Aborted compression: %d/%d: %d%%"),
	       produced, consumed,
	       (100 * produced) / consumed);
	  goto err;
	}
	
      if (evbuffer_add (target, buffer, produced) < 0)
	{
	  (void)deflateEnd(&strm);
	  goto err;
	}
    }
  while (strm.avail_out == 0);
  assert(strm.avail_in == 0);     /* all input will be used */
  assert(ret == Z_STREAM_END);        /* stream will be complete */

  /* clean up and return */
  (void)deflateEnd(&strm);

  return target;

 err:
  evbuffer_free (target);
  return NULL;
}
