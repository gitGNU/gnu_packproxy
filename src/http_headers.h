/* http_headers.h
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

#ifndef HTTP_HEADERS
#define HTTP_HEADERS

#define obstack_chunk_alloc malloc
#define obstack_chunk_free free
#include <obstack.h>

struct http_header
{
  short key_len;
  char *key;
  char *value;

  struct http_header *next;
};

struct http_headers
{
  struct http_header *head;
  struct obstack data;
};

extern struct http_headers *http_headers_new (const char *headers);

extern void http_headers_free (struct http_headers *headers);

/* Return the value of the header with key KEY.  Returns NULL if there
   is no such header.  */
const char *http_headers_find (struct http_headers *h, const char *key);

#endif
