/*
 * HashUtilStub: the Windows replacement of HashUtil.c. The Windows
 * build has no PuTTY core, so file digests are not available and the
 * checksum command reports that explicitly instead of silently doing
 * nothing.
 *
 * The one-shot SHA-256 and HMAC-SHA256 primitives are implemented here
 * instead of being stubbed, because the Amazon S3 client signs every
 * request with AWS Signature Version 4 on every platform: without
 * these, S3 sessions would not authenticate on Windows. The same
 * cross-platform test vectors (AWS documentation examples and the
 * botocore-signed requests of the live suite) hold this copy to the
 * same bytes as the PuTTY-backed one in HashUtil.c.
 */

#include <stdio.h>
#include <string.h>

#include "HashUtil.h"

/* ---- SHA-256 (FIPS 180-4), self-contained ------------------------------- */

namespace {

struct Sha256Ctx
{
    unsigned int h[8];
    unsigned long long bytes;
    unsigned char block[64];
    size_t used;
};

unsigned int Rotr(unsigned int x, int n)
{
    return (x >> n) | (x << (32 - n));
}

void Sha256Init(Sha256Ctx * ctx)
{
    static const unsigned int init[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->h, init, sizeof(init));
    ctx->bytes = 0;
    ctx->used = 0;
}

void Sha256Block(Sha256Ctx * ctx, const unsigned char * block)
{
    static const unsigned int k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
    {
        w[i] = (static_cast<unsigned int>(block[i * 4]) << 24) |
               (static_cast<unsigned int>(block[i * 4 + 1]) << 16) |
               (static_cast<unsigned int>(block[i * 4 + 2]) << 8) |
               static_cast<unsigned int>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++)
    {
        const unsigned int s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const unsigned int s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned int a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    unsigned int e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (int i = 0; i < 64; i++)
    {
        const unsigned int s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const unsigned int ch = (e & f) ^ (~e & g);
        const unsigned int t1 = h + s1 + ch + k[i] + w[i];
        const unsigned int s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        const unsigned int t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void Sha256Update(Sha256Ctx * ctx, const void * data, size_t len)
{
    const unsigned char * p = static_cast<const unsigned char *>(data);
    ctx->bytes += len;
    while (len > 0)
    {
        const size_t take = sizeof(ctx->block) - ctx->used < len ?
            sizeof(ctx->block) - ctx->used : len;
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == sizeof(ctx->block))
        {
            Sha256Block(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void Sha256Final(Sha256Ctx * ctx, unsigned char * out)
{
    const unsigned long long bits = ctx->bytes * 8;
    const unsigned char pad = 0x80;
    Sha256Update(ctx, &pad, 1);
    const unsigned char zero = 0;
    while (ctx->used != 56)
    {
        Sha256Update(ctx, &zero, 1);
    }
    unsigned char len[8];
    for (int i = 0; i < 8; i++)
    {
        len[i] = static_cast<unsigned char>(bits >> (56 - i * 8));
    }
    Sha256Update(ctx, len, 8);
    for (int i = 0; i < 8; i++)
    {
        for (int b = 0; b < 4; b++)
        {
            out[i * 4 + b] = static_cast<unsigned char>(
                ctx->h[i] >> (24 - b * 8));
        }
    }
}

void Sha256(const void * data, size_t len, unsigned char * out)
{
    Sha256Ctx ctx;
    Sha256Init(&ctx);
    Sha256Update(&ctx, data, len);
    Sha256Final(&ctx, out);
}

void HmacSha256(const void * key, size_t keyLen,
    const void * data, size_t dataLen, unsigned char * out)
{
    unsigned char keyBlock[64];
    memset(keyBlock, 0, sizeof(keyBlock));
    if (keyLen > sizeof(keyBlock))
    {
        Sha256(key, keyLen, keyBlock);
    }
    else
    {
        memcpy(keyBlock, key, keyLen);
    }
    unsigned char ipad[64];
    unsigned char opad[64];
    for (int i = 0; i < 64; i++)
    {
        ipad[i] = static_cast<unsigned char>(keyBlock[i] ^ 0x36);
        opad[i] = static_cast<unsigned char>(keyBlock[i] ^ 0x5c);
    }
    unsigned char inner[32];
    Sha256Ctx ctx;
    Sha256Init(&ctx);
    Sha256Update(&ctx, ipad, sizeof(ipad));
    Sha256Update(&ctx, data, dataLen);
    Sha256Final(&ctx, inner);
    unsigned char outer[32 + 64];
    Sha256Init(&ctx);
    Sha256Update(&ctx, opad, sizeof(opad));
    Sha256Update(&ctx, inner, sizeof(inner));
    Sha256Final(&ctx, outer);
    memcpy(out, outer, 32);
    memset(keyBlock, 0, sizeof(keyBlock));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
}

} // namespace

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

extern "C" int lscp_hmac_sha256(const void * key, size_t keyLen,
    const void * data, size_t dataLen, unsigned char * out, size_t outSize)
{
    if (out == NULL || outSize < 32 || key == NULL || data == NULL)
    {
        return -1;
    }
    HmacSha256(key, keyLen, data, dataLen, out);
    return 0;
}

extern "C" int lscp_sha256(const void * data, size_t dataLen,
    unsigned char * out, size_t outSize)
{
    if (out == NULL || outSize < 32 || data == NULL)
    {
        return -1;
    }
    Sha256(data, dataLen, out);
    return 0;
}
