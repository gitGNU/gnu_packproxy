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
