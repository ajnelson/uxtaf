#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.67])
AC_INIT([uxtaf], [0.1.0], [r.c.ladan@gmail.com])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/uxtaf.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([ssl], [SSL_library_init], [],
             [AC_MSG_NOTICE([cannot find openssl ssl lib; SHA1s of file data will not be calculated.])])
AC_CHECK_LIB([crypto], [EVP_EncryptInit], [],
             [AC_MSG_FAILURE([cannot find openssl crypto lib; SHA1s of file data will not be calculated.])])

# Checks for header files.
AC_CHECK_HEADERS([limits.h stdint.h stdlib.h string.h strings.h sys/utsname.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_FSEEKO
AC_CHECK_FUNCS([bzero strdup uname])

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT
