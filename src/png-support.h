/* png.h - PNG support.
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

#ifndef PNG_SUPPORT_H
#define PNG_SUPPORT_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>

/* Recompress the PNG stored in SOURCE.  QUALITY is the desired
   quality.  It should be between 0 and 100.  Returns NULL on failure,
   otherwise a buffer containing the image data.  */
extern struct evbuffer *png_recompress (struct evbuffer *source,
					int quality);

#endif
