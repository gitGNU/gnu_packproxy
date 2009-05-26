/* png-support.c - Support for handling PNG images.
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
#include <stdlib.h>
#include <assert.h>
#include <png.h>
#include <stdbool.h>

#include "png-support.h"
#include "log.h"

struct data
{
  png_structp ipng;
  png_infop iinfo;
  png_structp opng;
  png_infop oinfo;

  /* Intermediate data (i.e., the bitmap).  */
  png_bytepp row_pointers;
  png_bytep buffer;

  struct evbuffer *output;

  unsigned long width, height;
  int bit_depth, color_type, interlace_type;
  /* The number of passes required to extract the bitmap.  If this is
     1, the image is not interlaced.  If it is greater than 1, then
     the image is interlaced.  */
  int input_passes;

  bool finished;
};

/* Called after the input block has been processed.  */
static void
info_callback (png_structp ipng_ptr, png_infop iinfo_ptr)
{
  struct data *data = (struct data *) png_get_progressive_ptr (ipng_ptr);
  assert (data);
  assert (data->ipng == ipng_ptr);

  png_get_IHDR (data->ipng, data->iinfo,
		&data->width, &data->height,
		&data->bit_depth, &data->color_type,
		&data->interlace_type, NULL, NULL);

  /* Set up the data transformations.  */

  /* Strip 16 bit/color files down to 8 bits/color.  */
  png_set_strip_16 (data->ipng);

  /* Expand paletted colors into true RGB triplets */
  if (data->color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb (data->ipng);

  /* Expand grayscale images to the full 8 bits from 1, 2, or 4
     bits/pixel.  */
  if (data->color_type == PNG_COLOR_TYPE_GRAY && data->bit_depth < 8)
    png_set_gray_1_2_4_to_8 (data->ipng);

  /* Turn on interlace handling.  */
  data->input_passes = png_set_interlace_handling (data->ipng);

  png_read_update_info (data->ipng, data->iinfo);


  log ("%ld x %ld x %d, passes: %d",
       data->width, data->height, data->bit_depth, data->input_passes);


  /* Set the image information.  */
  png_set_IHDR (data->opng, data->oinfo, data->width, data->height,
		data->bit_depth, data->color_type,
		// Don't interlace yet... PNG_INTERLACE_ADAM7
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  /* Use same significant bit chunk */
  if (png_get_valid (data->ipng, data->iinfo, PNG_INFO_sBIT))
    {
      png_color_8p sig_bit;

      png_get_sBIT (data->ipng, data->iinfo, &sig_bit);
      png_set_sBIT(data->opng, data->oinfo, sig_bit);
    }

  /* Use the same gamma correction.  */
  double gamma;
  if (png_get_gAMA (data->ipng, data->iinfo, &gamma))
    png_set_gAMA (data->opng, data->oinfo, gamma);

  /* Write the file header information. */
  png_write_info (data->opng, data->oinfo);

  /* Set up transformations.  */

  /* Pack pixels into bytes.  */
  png_set_packing (data->opng);

  /* Get rid of filler (OR ALPHA) bytes, pack XRGB/RGBX/ARGB/RGBA into
     RGB (4 channels -> 3 channels). The second parameter is not
     used.  */ 
  png_set_filler (data->opng, 0, PNG_FILLER_BEFORE);



  /* Prepare for the transform.  */
  if (data->input_passes > 1)
    /* The source data is interlaced.  We need to buffer the data.  */
    {
      /* The easiest way to read the image: */
      data->row_pointers = malloc (sizeof (png_bytep) * data->height);
      int bytes_per_row = png_get_rowbytes (data->ipng, data->iinfo);
      data->buffer = malloc (sizeof (png_byte) * data->height * bytes_per_row);

      /* Set up the array.  */
      int row;
      for (row = 0; row < data->height; row ++)
	data->row_pointers[row] = &data->buffer[row * bytes_per_row];
    }

  /* Enable interlaced output?  In theory it would be nice but it is
     more complicated.  */
  // int output_passes = png_set_interlace_handling (opng);
}


/* Called for each scan line.  */
static void
row_callback (png_structp png_ptr, png_bytep new_row,
	      png_uint_32 row_num, int pass)
{
  struct data *data = (struct data *) png_get_progressive_ptr (png_ptr);
  assert (data);
  assert (data->ipng == png_ptr);

  if (data->input_passes > 1)
    /* We're dealing with an interlaced source image.  We need to
       combine the rows and only during the last pass can we write the
       data out.  */
    {
      png_progressive_combine_row (png_ptr, data->row_pointers[row_num],
				   new_row);

      if (pass == data->input_passes)
	png_write_rows (data->opng, data->row_pointers, 1);
    }
  else
    /* The data is not interlaced.  We can directly write NEW_ROW
       out.  */
    {
      png_bytep row_pointers[1];
      row_pointers[0] = new_row;

      png_write_rows (data->opng, row_pointers, 1);
    }

  log ("row: %d, pass: %d", (int) row_num, (int) pass);
}

/* Called after the image has been fully decoded.  */
static void
end_callback (png_structp png_ptr, png_infop info_ptr)
{
  struct data *data = (struct data *) png_get_progressive_ptr (png_ptr);
  assert (data);
  assert (data->ipng == png_ptr);
  assert (data->iinfo == info_ptr);

  png_write_end (data->opng, data->oinfo);

  assert (! data->finished);
  data->finished = true;
}


/* Called with produced data.  */
static void
output_write (png_structp png_ptr, png_bytep buffer, png_size_t length)
{
  struct data *data = (struct data *) png_get_io_ptr (png_ptr);
  assert (data);
  assert (data->opng == png_ptr);

  evbuffer_add (data->output, buffer, length);

  log ("%d (%d)", length, EVBUFFER_LENGTH (data->output));
}


struct evbuffer *
png_recompress (struct evbuffer *source, int quality)
{
  if (! png_check_sig (EVBUFFER_DATA (source), EVBUFFER_LENGTH (source)))
    {
      log ("Not a PNG file: signature mismatch.");
      return NULL;
    }

  struct evbuffer *ret = NULL;
  struct data data;
  memset (&data, 0, sizeof (data));

  /* Set up the reader.  */
  data.ipng = png_create_read_struct (PNG_LIBPNG_VER_STRING,
				      NULL, NULL, NULL);
  if (! data.ipng)
    goto err;

  data.iinfo = png_create_info_struct (data.ipng);
  if (! data.iinfo)
    goto err;

  if (setjmp (png_jmpbuf (data.ipng)))
    goto err;

  /* We use the progressive version as this allows us to easily supply
     the input data to the PNG routines directly, i.e., there is no
     need to copy the data to library provided buffers as is the case
     when using the I/O callbacks.  */
  png_set_progressive_read_fn (data.ipng, &data,
			       info_callback, row_callback, end_callback);


  /* Set up the writer.  */
  data.opng = png_create_write_struct (PNG_LIBPNG_VER_STRING,
					NULL, NULL, NULL);
  if (! data.opng)
    goto err;

  data.oinfo = png_create_info_struct (data.opng);
  if (! data.oinfo)
    goto err;

  if (setjmp (png_jmpbuf (data.opng)))
    goto err;

  data.output = evbuffer_new ();
  if (! data.output)
    goto err;

  png_set_write_fn (data.opng, (void *) &data, output_write, NULL);


  /* Process the data.  */
  png_process_data (data.ipng, data.iinfo,
		    EVBUFFER_DATA (source), EVBUFFER_LENGTH (source));

  if (data.finished)
    {
      ret = data.output;
      /* Don't free it.  */
      data.output = NULL;
    }

 err:
  if (data.row_pointers)
    free (data.row_pointers);
  if (data.buffer)
    free (data.buffer);

  if (data.opng)
    png_destroy_write_struct (&data.opng, data.oinfo ? &data.oinfo : NULL);
  if (data.output)
    evbuffer_free (data.output);
  png_destroy_read_struct (&data.ipng, data.iinfo ? &data.iinfo : NULL, NULL);

  return ret;
}
