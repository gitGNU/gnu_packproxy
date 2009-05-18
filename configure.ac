AC_INIT(src/main.c)
AM_INIT_AUTOMAKE(ziproxy-ng, 2.7.0)

AC_PROG_CC

AC_CHECK_LIB(event, event_init,, AC_MSG_ERROR([libevent not found.]))
AC_CHECK_LIB(z, gzopen64,, AC_MSG_ERROR([zlib not found.]))

AC_OUTPUT(Makefile
	 src/Makefile)
