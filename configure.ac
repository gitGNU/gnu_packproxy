AC_INIT([packproxy],[0.0], [neal@walfield.org])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/user_conn.c])

AC_PROG_CC

AC_CHECK_LIB(event, event_init,, AC_MSG_ERROR([libevent not found.]))
AC_CHECK_LIB(z, gzopen64,, AC_MSG_ERROR([zlib not found.]))
AC_CHECK_LIB(jpeg, jpeg_resync_to_restart,,
		   AC_MSG_ERROR([libjpeg62 not found.]))
AC_CHECK_LIB(png, png_get_channels,,
		   AC_MSG_ERROR([libpng not found.]))
AC_CHECK_LIB(sqlite3, sqlite3_libversion,, 
		   AC_MSG_ERROR([libsqlite3 not found.]))

AC_OUTPUT(Makefile
	 src/Makefile)
