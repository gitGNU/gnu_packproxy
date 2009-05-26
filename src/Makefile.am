AM_CFLAGS = -Wall
AM_CPPFLAGS = -D_GNU_SOURCE

bin_PROGRAMS = packproxy

packproxy_SOURCES = main.c \
	user_conn.h user_conn.c \
	http_conn.h http_conn.c \
	http_request.h http_request.c \
	http_response.h http_response.c \
	http_message.h http_message.c \
	http_headers.h http_headers.c \
	gzip.h gzip.c \
	jpeg.h jpeg.c \
	png-support.h png-support.c \
	list.h \
	log.h \
	opts.c opts.h \
	\
	http.c http-internal.h strlcpy-internal.h sys/tree.h

