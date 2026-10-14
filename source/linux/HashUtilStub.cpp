/*
 * HashUtilStub: the Windows replacement of HashUtil.c. The Windows
 * build has no PuTTY core, so local digests are not available; the
 * checksum command reports that explicitly instead of silently doing
 * nothing.
 */

#include <stdio.h>
#include <string.h>

#include "HashUtil.h"

extern "C" int lscp_hash_file(const char * algorithm, const char * path,
    char * hex, size_t hexSize)
{
    (void)algorithm;
    (void)path;
    if (hex != NULL && hexSize > 0)
    {
        hex[0] = '\0';
    }
    return -1;
}

extern "C" const char * lscp_hash_error(void)
{
    return "Local checksum calculation is not available in this build.";
}
