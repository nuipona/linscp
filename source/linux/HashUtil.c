#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"

#include "HashUtil.h"

#define HASH_CHUNK 65536

static char g_error[512];

static void set_error(const char * text)
{
    snprintf(g_error, sizeof(g_error), "%s", text);
}

const char * lscp_hash_error(void)
{
    return g_error;
}

/* CRC-32 (IEEE 802.3), the polynomial zlib and WinSCP use. */
static uint32_t lscp_crc32_update(uint32_t crc, const unsigned char * data,
    size_t len)
{
    static uint32_t table[256];
    static int tableReady = 0;
    if (!tableReady)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t value = i;
            for (int bit = 0; bit < 8; bit++)
            {
                value = (value & 1) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            table[i] = value;
        }
        tableReady = 1;
    }
    crc ^= 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
    {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

int lscp_hash_file(const char * algorithm, const char * path,
    char * hex, size_t hexSize)
{
    g_error[0] = '\0';
    if (hex == NULL || hexSize == 0)
    {
        set_error("No output buffer was given.");
        return -1;
    }
    hex[0] = '\0';

    const ssh_hashalg * alg = NULL;
    uint32_t crc = 0;
    char normalized[32];
    {
        size_t n = 0;
        for (size_t i = 0; algorithm != NULL && algorithm[i] != '\0' && n + 1 < sizeof(normalized); i++)
        {
            const char c = algorithm[i];
            if (c == '-' || c == '_')
            {
                continue;
            }
            normalized[n++] = (char)tolower((unsigned char)c);
        }
        normalized[n] = '\0';
        if (strcmp(normalized, "sha256") == 0)
        {
            alg = &ssh_sha256;
        }
        else if (strcmp(normalized, "sha1") == 0)
        {
            alg = &ssh_sha1;
        }
        else if (strcmp(normalized, "md5") == 0)
        {
            alg = &ssh_md5;
        }
        else if (strcmp(normalized, "crc32") == 0)
        {
            crc = 0;
        }
        else
        {
            set_error("Unknown algorithm; use sha-256, sha-1, md5 or crc32.");
            return -1;
        }
    }

    FILE * file = fopen(path, "rb");
    if (file == NULL)
    {
        set_error("Cannot open the file.");
        return -1;
    }

    ssh_hash * hash = alg != NULL ? ssh_hash_new(alg) : NULL;
    unsigned char buffer[HASH_CHUNK];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) != 0)
    {
        if (alg != NULL)
        {
            put_data(hash, buffer, got);
        }
        else
        {
            crc = lscp_crc32_update(crc, buffer, got);
        }
    }
    const bool readFailed = ferror(file) != 0;
    fclose(file);
    if (readFailed)
    {
        if (hash != NULL)
        {
            ssh_hash_free(hash);
        }
        set_error("Cannot read the file.");
        return -1;
    }

    unsigned char digest[64];
    size_t digestLen = 0;
    if (alg != NULL)
    {
        digestLen = alg->hlen;
        if (digestLen > sizeof(digest))
        {
            ssh_hash_free(hash);
            set_error("The digest is too large.");
            return -1;
        }
        ssh_hash_final(hash, digest);
    }
    else
    {
        digest[0] = (unsigned char)(crc >> 24);
        digest[1] = (unsigned char)(crc >> 16);
        digest[2] = (unsigned char)(crc >> 8);
        digest[3] = (unsigned char)(crc);
        digestLen = 4;
    }

    static const char digits[] = "0123456789abcdef";
    size_t out = 0;
    for (size_t i = 0; i < digestLen && out + 2 < hexSize; i++)
    {
        hex[out++] = digits[digest[i] >> 4];
        hex[out++] = digits[digest[i] & 0x0f];
    }
    hex[out] = '\0';
    return 0;
}

int lscp_hmac_sha256(const void * key, size_t keyLen,
    const void * data, size_t dataLen, unsigned char * out, size_t outSize)
{
    if (out == NULL || outSize < 32)
    {
        set_error("The HMAC output buffer is too small.");
        return -1;
    }
    mac_simple(&ssh_hmac_sha256, make_ptrlen(key, keyLen),
        make_ptrlen(data, dataLen), out);
    return 0;
}
