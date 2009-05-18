AC_INIT([ziproxy-ng],[2.7.0], [neal@walfield.org])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/main.c])

AC_PROG_CC

AC_CHECK_LIB(event, event_init,, AC_MSG_ERROR([libevent not found.]))
AC_CHECK_LIB(z, gzopen64,, AC_MSG_ERROR([zlib not found.]))
AC_CHECK_LIB(jpeg, jpeg_resync_to_restart,,
		   AC_MSG_ERROR([libjpeg62 not found.]))

AC_OUTPUT(Makefile
	 src/Makefile)
