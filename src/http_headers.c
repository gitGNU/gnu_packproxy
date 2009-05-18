#include <stdlib.h>
#include <stdint.h>

#include "http_headers.h"
#include "log.h"

struct http_headers *
http_headers_new (const char *headers)
{
  struct http_headers *h = calloc (sizeof (*h), 1);

  obstack_init (&h->data);

  char *colon;
  while (headers && (colon = strchr (headers, ':')))
    {
      const char *key = headers;

      struct http_header *header
	= obstack_alloc (&h->data, sizeof (struct http_header));

      int len = (intptr_t) colon - (intptr_t) key;
      header->key = obstack_copy0 (&h->data, key, len);
      header->key_len = len;

      headers = colon + 1;
      if (*headers == ' ')
	headers ++;

      const char *value = headers;

      const char *end = strchr (headers, '\n');
      len = 0;
      if (end)
	{
	  if (end[-1] == '\r')
	    len = -1;
	  len += (intptr_t) end - (intptr_t) value;

	  headers = end + 1;
	}
      else
	{
	  headers = NULL;
	  len = strlen (value);
	}

      header->value = obstack_copy0 (&h->data, value, len);

      log ("Found %s: %s", header->key, header->value);

      header->next = h->head;
      h->head = header;
    }

  return h;
}

void
http_headers_free (struct http_headers *h)
{
  obstack_free (&h->data, NULL);
  free (h);
}

const char *
http_headers_find (struct http_headers *h, const char *key)
{
  int len = strlen (key);

  struct http_header *header;
  for (header = h->head; header; header = header->next)
    if (len == header->key_len
	&& strcasecmp (header->key, key) == 0)
      return header->value;
  return NULL;
}

