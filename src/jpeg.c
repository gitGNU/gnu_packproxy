/* jpeg.c
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
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <jpeglib.h>
#include <alloca.h>
#include <setjmp.h>

#include "log.h"

static void
init_source (j_decompress_ptr cinfo)
{
}

static boolean
fill_input_buffer (j_decompress_ptr cinfo)
{
  /* Hmm.  In practice, this should never be called.  The fact that
     the decompressor is looking for more data suggests that this is a
     bad image.  Insert a fake end of input marker.  */
  static JOCTET eoi[2];
  eoi[0] = (JOCTET) 0xFF;
  eoi[1] = (JOCTET) JPEG_EOI;

  cinfo->src->next_input_byte = &eoi[0];
  cinfo->src->bytes_in_buffer = sizeof (eoi);

  return TRUE;
}

static void
skip_input_data (j_decompress_ptr cinfo, long num_bytes)
{
  if (num_bytes <= 0)
    return;

  if (num_bytes <= cinfo->src->bytes_in_buffer)
    {
      cinfo->src->next_input_byte += (size_t) num_bytes;
      cinfo->src->bytes_in_buffer -= num_bytes;
    }
  else
    cinfo->src->bytes_in_buffer = 0;
}

static void
term_source (j_decompress_ptr cinfo)
{
}

struct my_destination_msg
{
  struct jpeg_destination_mgr pub;
  struct evbuffer *buffer;
  unsigned char *pos;
  size_t source_size;
};

#define CHUNK (4096 * 16)

#define EVBUFFER_AVAILABLE(buf) ((buf)->totallen - (buf)->misalign)

static void
init_destination (j_compress_ptr cinfo)
{
  struct my_destination_msg *dest = (struct my_destination_msg *) cinfo->dest;

  assert (dest->buffer);
  assert (EVBUFFER_LENGTH (dest->buffer) == 0);

  /* We allocate the same number of bytes as are in the source image.
     The output will (hopefully) be less, however, we would really
     like to avoid a memcpy, which would be required if we have to
     grow the buffer.  Any used bits shouldn't really hurt us.  */
  /* NB: Expand doesn't actually update the length.  */
  evbuffer_expand (dest->buffer, dest->source_size);

  dest->pub.next_output_byte = dest->pos = EVBUFFER_DATA (dest->buffer);
  dest->pub.free_in_buffer = EVBUFFER_AVAILABLE (dest->buffer);
}

static boolean
empty_output_buffer (j_compress_ptr cinfo)
{
  /* Hmm.  If we get here, that means that the new image is larger
     than the original.  It is perhaps better to just abort.  */
  struct my_destination_msg *dest = (struct my_destination_msg *) cinfo->dest;

  assert (dest->pub.free_in_buffer == 0);

  int written = (intptr_t) dest->pub.next_output_byte - (intptr_t) dest->pos;
  assert (written > 0);
  assert (written <= EVBUFFER_AVAILABLE (dest->buffer));

  /* evbuffer_expand does not adjust the length.  Do it now.  */
  EVBUFFER_LENGTH (dest->buffer) += written;

  evbuffer_expand (dest->buffer, EVBUFFER_LENGTH (dest->buffer) + CHUNK);

  dest->pub.next_output_byte
    = dest->pos
    = EVBUFFER_DATA (dest->buffer) + EVBUFFER_LENGTH (dest->buffer);
  dest->pub.free_in_buffer
    = EVBUFFER_AVAILABLE (dest->buffer) - EVBUFFER_LENGTH (dest->buffer);
  assert (dest->pub.free_in_buffer >= CHUNK);

  return TRUE;
}

static void
term_destination (j_compress_ptr cinfo)
{
  /* Hmm.  If we get here, that means that the new image is larger
     than the original.  It is perhaps better to just abort.  */
  struct my_destination_msg *dest = (struct my_destination_msg *) cinfo->dest;

  int written = (intptr_t) dest->pub.next_output_byte - (intptr_t) dest->pos;
  assert (written <= EVBUFFER_AVAILABLE (dest->buffer));

  /* evbuffer_expand does not adjust the length.  Do it now.  */
  EVBUFFER_LENGTH (dest->buffer) += written;
}


struct my_error_mgr
{
  struct jpeg_error_mgr jpeg_error_mgr;
  jmp_buf *jmp_bufp;
};

static void
error_exit (j_common_ptr cinfo)
{
  /* Emit the error message.  */
  (*cinfo->err->output_message) (cinfo);

  struct my_error_mgr *error_mgr = (struct my_error_mgr *) cinfo->err;
  longjmp (*error_mgr->jmp_bufp, 1);
}


struct evbuffer *
jpeg_recompress (struct evbuffer *source, int quality)
{
  struct jpeg_decompress_struct *decompressp = NULL;
  struct jpeg_compress_struct *compressp = NULL;

  struct jpeg_decompress_struct decompress;
  struct my_error_mgr error_mgr;
  jpeg_std_error (&error_mgr.jpeg_error_mgr);

  jmp_buf jmp_buf;
  error_mgr.jmp_bufp = &jmp_buf;
  error_mgr.jpeg_error_mgr.error_exit = error_exit;

  if (setjmp (jmp_buf))
    {
      if (decompressp)
	jpeg_destroy_decompress (decompressp);

      if (compressp)
	{
	  if (compressp->dest)
	    {
	      struct my_destination_msg *dest
		= (struct my_destination_msg *) compressp->dest;
	      evbuffer_free (dest->buffer);
	    }

	  jpeg_destroy_compress (compressp);
	}

      return NULL;
    }
  

  /* Set up the decompresser.  */
  decompress.err = &error_mgr.jpeg_error_mgr;
  jpeg_create_decompress (&decompress);
  decompressp = &decompress;

  /* Options for fast (quick 'n dirty) decompression.  */
  decompress.two_pass_quantize = FALSE;
  decompress.dither_mode = JDITHER_ORDERED;
  decompress.desired_number_of_colors = 216;
  decompress.dct_method = JDCT_FASTEST;
  decompress.do_fancy_upsampling = FALSE;

  struct jpeg_source_mgr src;
  decompress.src = &src;

  src.next_input_byte = EVBUFFER_DATA (source);
  src.bytes_in_buffer = EVBUFFER_LENGTH (source);

  src.init_source = init_source;
  src.fill_input_buffer = fill_input_buffer;
  src.skip_input_data = skip_input_data;
  src.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src.term_source = term_source;

  /* This determines the image width, height, components and color
     space.  */
  jpeg_read_header (&decompress, true);
  /* This computes the output width, height, compnents, etc. based on
     the parameters.  We require this information to set up the
     compressor.  */
  jpeg_start_decompress (&decompress);

  log ("Image: %d x %d @ %d, %d bytes",
       decompress.image_width, decompress.image_height,
       decompress.num_components,
       src.bytes_in_buffer);

  if (decompress.image_width > 6000 || decompress.image_height > 6000)
    /* The image is unusually (and perhaps suspiciously?) large.  Even
       if it is legitimate, it will take a long time to recompress.
       Don't even try.  */
    {
      log ("Image suspiciously large, not recompressing.");
      jpeg_destroy_decompress (&decompress);
      return NULL;
    }


  /* Set up the compressor.  */
  struct jpeg_compress_struct compress;
  /* Set COMPRESS.DEST to NULL.  Then, in case we error out in
     jpeg_create_compress, we know in the error handler that
     DEST.BUFFER has not yet been initialized.  */
  compress.dest = NULL;
  compress.err = &error_mgr.jpeg_error_mgr;
  jpeg_create_compress (&compress);
  compressp = &compress;

  struct my_destination_msg dest;
  compress.dest = &dest.pub;
  dest.pub.init_destination = init_destination;
  dest.pub.empty_output_buffer = empty_output_buffer;
  dest.pub.term_destination = term_destination;
  dest.pub.free_in_buffer = 0;

  dest.buffer = evbuffer_new ();
  dest.source_size = EVBUFFER_LENGTH (source);

  /* IN_COLOR_SPACE must be set prior to calling jpeg_set_defaults.  */
  compress.in_color_space = decompress.out_color_space;
  jpeg_set_defaults (&compress);

  compress.image_width = decompress.output_width;
  compress.image_height = decompress.output_height;
  compress.input_components = decompress.output_components;

  /* Use fast integer encoding--the least accurate.  */
  compress.dct_method = JDCT_IFAST;
  /* Set the quality appropriately.  */
  jpeg_set_quality (&compress, quality, TRUE);
  /* Use progressive encoding.  */
  jpeg_simple_progression (&compress);

  jpeg_start_compress(&compress, TRUE);


  /* Begin recompression.  */
  int w = decompress.output_width;
  int d = decompress.output_components;
  int scanlines = decompress.rec_outbuf_height;

  unsigned char *buf = alloca (w * d * sizeof (JSAMPLE) * scanlines);
  unsigned char *ibuf[scanlines];
  int i;
  for (i = 0; i < scanlines; i ++)
    ibuf[i] = buf + w * d * i;

  while (decompress.output_scanline < decompress.output_height)
    {
      int scanlines = decompress.rec_outbuf_height;
      scanlines = jpeg_read_scanlines (&decompress,
				       (JSAMPARRAY) &ibuf, scanlines);
      jpeg_write_scanlines (&compress, (JSAMPARRAY) &ibuf, scanlines);
    }

  jpeg_finish_decompress(&decompress);
  jpeg_destroy_decompress(&decompress);

  jpeg_finish_compress(&compress);
  jpeg_destroy_compress(&compress);

  return dest.buffer;
}
