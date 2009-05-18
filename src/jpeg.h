#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>

/* Recompress the JPEG stored in SOURCE.  QUALITY is the desired
   quality.  It should be between 0 and 100.  Returns NULL on failure,
   otherwise a buffer containing the image data.  */
extern struct evbuffer *jpeg_recompress (struct evbuffer *source,
					 int quality);
