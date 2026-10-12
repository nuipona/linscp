/* expat_config.h - hand-maintained configuration for the Windows (C++Builder 11) build.

   This replaces the output of the POSIX `configure` script, which enabled
   POSIX-only features (getentropy, dlfcn, mmap, unistd, sys/param). The
   Windows build compiles lib/*.c with FORK_BUILD and takes its entropy from
   random_rand_s.c (RtlGenRandom), so none of those features apply here.
   Do not regenerate this file with configure. */

#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

/* Define if building universal (internal helper macro) */
/* #undef AC_APPLE_UNIVERSAL_BUILD */

/* 1234 = LILENDIAN, 4321 = BIGENDIAN */
#define BYTEORDER 1234

/* arc4random / arc4random_buf do not exist on Windows */
// FORK_BUILD
/* #define HAVE_ARC4RANDOM 1 */
// FORK_BUILD
/* #undef HAVE_ARC4RANDOM_BUF */

/* define if the compiler supports basic C++11 syntax */
#define HAVE_CXX11 1

/* POSIX-only: <dlfcn.h> is not available on Windows */
/* #undef HAVE_DLFCN_H */

/* <fcntl.h> is available through the MSVC/C++Builder CRT */
#define HAVE_FCNTL_H 1

/* POSIX-only: getentropy is not available on Windows */
/* #undef HAVE_GETENTROPY */

/* POSIX-only: getpagesize is not available on Windows */
/* #undef HAVE_GETPAGESIZE */

/* POSIX-only: getrandom is not available on Windows */
/* #undef HAVE_GETRANDOM */

#define HAVE_INTTYPES_H 1

/* POSIX-only: mmap is not used on Windows */
/* #undef HAVE_MMAP */

#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

/* POSIX-only: <strings.h> is not available on Windows */
/* #undef HAVE_STRINGS_H */

/* POSIX-only: syscall(SYS_getrandom) does not exist on Windows */
/* #undef HAVE_SYSCALL_GETRANDOM */

/* POSIX-only: <sys/param.h> is not available on Windows */
/* #undef HAVE_SYS_PARAM_H */

#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1

/* POSIX-only: <unistd.h> is not available on Windows */
/* #undef HAVE_UNISTD_H */

/* Define to the sub-directory where libtool stores uninstalled libraries. */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "expat"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "https://github.com/libexpat/libexpat/issues"

/* Define to the full name of this package. */
#define PACKAGE_NAME "expat"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "expat 2.9.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "expat"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "2.9.0"

/* Define to 1 if all of the C89 standard headers exist. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "2.9.0"

/* Define WORDS_BIGENDIAN to 1 if your processor stores words with the most
   significant byte first (like Motorola and SPARC, unlike Intel). */
#if defined AC_APPLE_UNIVERSAL_BUILD
# if defined __BIG_ENDIAN__
#  define WORDS_BIGENDIAN 1
# endif
#else
# ifndef WORDS_BIGENDIAN
/* #  undef WORDS_BIGENDIAN */
# endif
#endif

/* Define to specify how much context to retain around the current parse
   point, 0 to disable. */
#define XML_CONTEXT_BYTES 1024

/* Define to include code reading entropy from `/dev/urandom'.
   POSIX-only; Windows uses rand_s (random_rand_s.c). */
/* #undef XML_DEV_URANDOM */

/* Define to make parameter entity parsing functionality available. */
#define XML_DTD 1

/* Define as 1/0 to enable/disable support for general entities. */
#define XML_GE 1

/* Define to make XML Namespaces functionality available. */
#define XML_NS 1

/* Define to empty if 'const' does not conform to ANSI C. */
/* #undef const */

/* Define to 'long int' if <sys/types.h> does not define. */
/* #undef off_t */

#endif // ndef EXPAT_CONFIG_H
