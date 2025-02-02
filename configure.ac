
AC_INIT
AM_INIT_AUTOMAKE(or, 0.0.0)
AM_CONFIG_HEADER(orconfig.h)

CFLAGS="-Wall -O2"

AC_ARG_ENABLE(debug,
[  --enable-debug          compiles with debugging info],
[if test x$enableval = xyes; then
    CFLAGS="-Wall -ggdb -O2"
fi])

AC_PROG_CC
AC_PROG_MAKE_SET
AC_PROG_RANLIB

AC_DEFINE([HAVE_OPENSSL], 0, [Define if OpenSSL was found.])
AC_DEFINE([HAVE_POPT], 0, [Define if popt was found.])

AC_CHECK_LIB(popt,poptParseArgvString,
	AC_DEFINE(HAVE_POPT),
	AC_MSG_ERROR(required library not found: popt)
)

LIBS="$LIBS -lpopt"

# The big search for OpenSSL
# copied from openssh's configure.ac
AC_ARG_WITH(ssl-dir,
        [  --with-ssl-dir=PATH     Specify path to OpenSSL installation ],
        [
                if test "x$withval" != "xno" ; then
                        tryssldir=$withval
                fi
        ]
)

saved_LIBS="$LIBS"
saved_LDFLAGS="$LDFLAGS"
saved_CPPFLAGS="$CPPFLAGS"
if test "x$prefix" != "xNONE" ; then
        tryssldir="$tryssldir $prefix"
fi
AC_CACHE_CHECK([for OpenSSL directory], ac_cv_openssldir, [
        for ssldir in $tryssldir "" /usr/local/openssl /usr/lib/openssl /usr/local/ssl /usr/lib/ssl /usr/local /usr/pkg /opt /opt/openssl ; do
                CPPFLAGS="$saved_CPPFLAGS"
                LDFLAGS="$saved_LDFLAGS"
                LIBS="$saved_LIBS -lcrypto"
                
                # Skip directories if they don't exist
                if test ! -z "$ssldir" -a ! -d "$ssldir" ; then
                        continue;
                fi
                if test ! -z "$ssldir" -a "x$ssldir" != "x/usr"; then
                        # Try to use $ssldir/lib if it exists, otherwise 
                        # $ssldir
                        if test -d "$ssldir/lib" ; then
                                LDFLAGS="-L$ssldir/lib $saved_LDFLAGS"
                                if test ! -z "$need_dash_r" ; then
                                        LDFLAGS="-R$ssldir/lib $LDFLAGS"
                                fi
                        else
                                LDFLAGS="-L$ssldir $saved_LDFLAGS"
                                if test ! -z "$need_dash_r" ; then
                                        LDFLAGS="-R$ssldir $LDFLAGS"
                                fi
                        fi
                        # Try to use $ssldir/include if it exists, otherwise 
                        # $ssldir
                        if test -d "$ssldir/include" ; then
                                CPPFLAGS="-I$ssldir/include $saved_CPPFLAGS"
                        else
                                CPPFLAGS="-I$ssldir $saved_CPPFLAGS"
                        fi
                fi

                # Basic test to check for compatible version and correct linking
                # *does not* test for RSA - that comes later.
                AC_TRY_RUN(
                        [
#include <string.h>
#include <openssl/rand.h>
int main(void) 
{
        char a[2048];
        memset(a, 0, sizeof(a));
        RAND_add(a, sizeof(a), sizeof(a));
        return(RAND_status() <= 0);
}
                        ],
                        [
                                found_crypto=1
                                break;
                        ], []
                )

                if test ! -z "$found_crypto" ; then
                        break;
                fi
        done

        if test -z "$found_crypto" ; then
                AC_MSG_ERROR([Could not find working OpenSSL library, please install or check config.log])      
        fi
        if test -z "$ssldir" ; then
                ssldir="(system)"
        fi

        ac_cv_openssldir=$ssldir
])
if (test ! -z "$ac_cv_openssldir" && test "x$ac_cv_openssldir" != "x(system)") ;
 then
        AC_DEFINE(HAVE_OPENSSL)
        dnl Need to recover ssldir - test above runs in subshell
        ssldir=$ac_cv_openssldir
        if test ! -z "$ssldir" -a "x$ssldir" != "x/usr"; then
                # Try to use $ssldir/lib if it exists, otherwise 
                # $ssldir
                if test -d "$ssldir/lib" ; then
                        LDFLAGS="-L$ssldir/lib $saved_LDFLAGS"
                        if test ! -z "$need_dash_r" ; then
                                LDFLAGS="-R$ssldir/lib $LDFLAGS"
                        fi
                else
                        LDFLAGS="-L$ssldir $saved_LDFLAGS"
                        if test ! -z "$need_dash_r" ; then
                                LDFLAGS="-R$ssldir $LDFLAGS"
                        fi
                fi
                # Try to use $ssldir/include if it exists, otherwise 
                # $ssldir
                if test -d "$ssldir/include" ; then
                        CPPFLAGS="-I$ssldir/include $saved_CPPFLAGS"
                else
                        CPPFLAGS="-I$ssldir $saved_CPPFLAGS"
                fi
        fi
fi
LIBS="$saved_LIBS -lcrypto"

AC_OUTPUT(Makefile src/Makefile src/common/Makefile src/smtpap/Makefile src/orkeygen/Makefile src/httpap/Makefile src/op/Makefile src/or/Makefile)

