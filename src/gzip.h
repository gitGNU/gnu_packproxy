/* gzip.h
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

#ifndef GZIP_H
#define GZIP_H

#include <sys/queue.h>
#include <sys/types.h>
#include <event.h>
#include <zlib.h>

/* gzip SOURCE.  Result is returned or NULL, if an error occured.

   If, while processing the first half of the stream, it appears that
   the result will be larger than MIN_PERCENT of the input, then
   compression is aborted.  After the half way point, the threshold is
   upped to MAX (97%, MIN_PRECENT).  On completion, it is set to MAX
   (99%, MIN_PERCENT).

   DEFLATE_FLAG makes the buffer be compressed in deflate-style rather
   than gzip-style when it is non-zero.
 */
struct evbuffer *evbuffer_gzip (struct evbuffer *source,
				int min_percent, int deflate_flag);

#endif
