#ifndef LINSCP_S3SIGV4_H
#define LINSCP_S3SIGV4_H

#include <string>
#include <utility>
#include <vector>

namespace linscp {

/*
 * AWS Signature Version 4, the signing scheme every Amazon S3 request
 * needs. Pure string work over the shared SHA-256/HMAC primitives, so
 * that the unit tests can hold it to the published AWS examples and to
 * requests signed by botocore without a server in the loop.
 *
 * The pieces:
 *   UriEncode          percent-encoding with the unreserved set of
 *                      RFC 3986 (the S3 flavor: '/' stays only where a
 *                      path says so)
 *   CanonicalQuery     sorted, encoded query string
 *   SigningKey         the date/region/service HMAC chain from the
 *                      secret access key
 *   CanonicalRequest   method, path, query, signed headers, payload
 *                      hash - exactly the bytes the server re-derives
 *   StringToSign       algorithm, timestamp, scope, canonical hash
 *   SignatureHex       HMAC of the string to sign under the key
 *   Authorization      the header curl sends
 */
namespace s3sig {

/* Percent-encode 'value'. keepSlash leaves '/' alone (paths); in
 * queries every reserved character including '/' is encoded. Uppercase
 * hex digits, '~' never encoded (SigV4's set, not the looser one). */
std::string UriEncode(const std::string & value, bool keepSlash);

/* Canonical form of a query string whose pairs are already encoded
 * ('k=v&k2=v2'): pairs sorted by name, then value. */
std::string CanonicalQuery(const std::string & query);

/* 'date/region/service/aws4_request' as it appears in the credential. */
std::string CredentialScope(const std::string & dateStamp,
    const std::string & region, const std::string & service);

/* The HMAC chain AWS4 + secret -> date -> region -> service ->
 * aws4_request: 32 raw bytes. */
std::string SigningKey(const std::string & secretAccessKey,
    const std::string & dateStamp, const std::string & region,
    const std::string & service);

/*
 * The canonical request. 'headers' are the headers being signed:
 * lowercase names, pre-trimmed, in any order (sorted here);
 * 'payloadHash' is the x-amz-content-sha256 value (hash of the body or
 * UNSIGNED-PAYLOAD). The result is exactly what a compliant server
 * reconstructs from the wire.
 */
std::string CanonicalRequest(const std::string & method,
    const std::string & uri, const std::string & query,
    const std::vector<std::pair<std::string, std::string> > & headers,
    const std::string & payloadHash);

/* 'AWS4-HMAC-SHA256\n<amzDate>\n<scope>\n<hex sha256 of canonical>'. */
std::string StringToSign(const std::string & amzDate,
    const std::string & scope, const std::string & canonicalRequest);

/* Lowercase hex HMAC-SHA256 of 'stringToSign' under 'signingKey' (32
 * raw bytes). */
std::string SignatureHex(const std::string & stringToSign,
    const std::string & signingKey);

/* The full Authorization header value: 'AWS4-HMAC-SHA256
 * Credential=<accessKey>/<scope>, SignedHeaders=<names>, Signature=<hex>'. */
std::string AuthorizationHeader(const std::string & accessKey,
    const std::string & scope, const std::string & signedHeaders,
    const std::string & signatureHex);

/* Time stamps: '20260115T103000Z' and '20260115' for 'when' (UTC). */
std::string AmzDate(unsigned long long epochSeconds);
std::string DateStamp(unsigned long long epochSeconds);

/* Lowercase hex of 32 raw bytes. */
std::string HexEncode(const std::string & raw);

/* Lowercase hex SHA-256 digest of 'data'. */
std::string Sha256Hex(const std::string & data);

/*
 * Convenience for the client: from method/path/query to the headers
 * curl must send. 'extras' are additional headers to sign (S3 requires
 * every x-amz-* header to be signed); this adds host, x-amz-date and
 * x-amz-content-sha256 (= 'payloadHash'). 'canonicalOut'/'signedOut'
 * are for tests and diagnostics.
 */
struct SignedRequest
{
    std::string authorization;
    std::string canonicalRequest;
    std::string signedHeaders; /* 'host;x-amz-content-sha256;...' */
    std::vector<std::pair<std::string, std::string> > headers;
};

SignedRequest Sign(const std::string & method, const std::string & uri,
    const std::string & query, const std::string & host,
    const std::vector<std::pair<std::string, std::string> > & extras,
    const std::string & payloadHash, unsigned long long epochSeconds,
    const std::string & region, const std::string & accessKey,
    const std::string & secretAccessKey);

} // namespace s3sig
} // namespace linscp

#endif /* LINSCP_S3SIGV4_H */
