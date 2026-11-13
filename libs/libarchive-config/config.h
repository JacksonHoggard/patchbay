/*
 * Hand-written libarchive configuration for the Wii U (devkitPPC + wut +
 * newlib). PatchBay only builds the read side of libarchive for 7-Zip and
 * RAR (v4 + v5), so only what those readers need is declared here. Disk
 * access, ACLs, xattrs, iconv and crypto libraries are left out on purpose.
 */
#ifndef PATCHBAY_LIBARCHIVE_CONFIG_H
#define PATCHBAY_LIBARCHIVE_CONFIG_H

/* Integer types (newlib provides all of C99 stdint). */
#define HAVE_INT16_T 1
#define HAVE_INT32_T 1
#define HAVE_INT64_T 1
#define HAVE_INTMAX_T 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
#define HAVE_UINTMAX_T 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1

/* Headers. */
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1

/* Functions. */
#define HAVE_EILSEQ 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_MBRTOWC 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_VPRINTF 1

/*
 * There is no iconv and newlib's "C" locale is ASCII, so libarchive would
 * turn every non-ASCII name into '?' (not even a legal FAT32 character).
 * The Wii U SD card takes UTF-8 names, so declare the "current locale" as
 * UTF-8: libarchive then uses its built-in UTF-16 -> UTF-8 converters for
 * 7-Zip and RAR names. archive_string.c is the only file built here that
 * calls nl_langinfo().
 */
#define HAVE_NL_LANGINFO 1
#define CODESET 0
#define nl_langinfo(item) ((void) (item), "UTF-8")

/* Compression libraries (devkitPro ppc portlibs). */
#define HAVE_ZLIB_H 1
#define HAVE_LIBZ 1
#define HAVE_LZMA_H 1
#define HAVE_LIBLZMA 1
#define HAVE_BZLIB_H 1
#define HAVE_LIBBZ2 1

#endif
