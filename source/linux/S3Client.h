#ifndef LINUXSCP_S3CLIENT_H
#define LINUXSCP_S3CLIENT_H

#include <string>

#include "Session.h"
#include "SftpClient.h"

namespace linuxscp {

/*
 * Amazon S3 over HTTPS with AWS Signature Version 4: a real bucket and
 * object filesystem on top of the flat object store - ListObjectsV2
 * with a delimiter for directories, zero-byte "key/" markers for
 * mkdir, copy-plus-delete for rename, HEAD for stat.
 *
 * Credentials are the site's user name (access key ID) and password
 * (secret access key), given as s3://ACCESSKEY:SECRET@host/bucket/ or
 * through -username=/-password=. The region comes from AWS_REGION or
 * AWS_DEFAULT_REGION, else from an s3.<region>.amazonaws.com host,
 * else us-east-1. Every request carries x-amz-content-sha256 (the hash
 * of an empty body, or UNSIGNED-PAYLOAD for uploads) because S3
 * requires that header to be signed. Small objects use PutObject; files
 * larger than 16 MiB use the S3 multipart API with an 8 MiB minimum part
 * size (scaled as needed to stay under 10,000 parts). On a handled
 * failure the client attempts to abort the multipart upload; there is no
 * persisted upload-ID/part state, so a retry restarts from the first part.
 *
 * The signing itself lives in S3SigV4.cpp (pure strings, unit-tested
 * against AWS's published examples); this file is the HTTP side: one
 * curl process per request, the response code parsed from
 * --write-out, the S3 error document read for its Code/Message when
 * something fails.
 */
class S3Client
{
public:
    static bool List(const SessionOptions & options,
        const std::string & remotePath, LscpNames * names,
        std::string & error);

    static int Stat(const SessionOptions & options,
        const std::string & remotePath, LscpAttrs * attrs,
        std::string & error);

    static int Get(const SessionOptions & options,
        const std::string & remotePath, const std::string & localPath,
        unsigned long long resumeOffset, bool preserveTime,
        unsigned long long * size, LscpProgressFn progress,
        void * progressCtx, std::string & error,
        unsigned speedKBytesPerSecond = 0);

    static int Put(const SessionOptions & options,
        const std::string & localPath, const std::string & remotePath,
        int permissions, unsigned long long resumeOffset,
        bool preserveTime, unsigned long long * size,
        LscpProgressFn progress, void * progressCtx, std::string & error,
        unsigned speedKBytesPerSecond = 0);

    static bool CheckEndpoint(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    static int Remove(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    static int RemoveDir(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    static int MakeDir(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    /* Returns 0 when a server-side CopyObject succeeds. */
    static int Copy(const SessionOptions & options,
        const std::string & fromPath, const std::string & toPath,
        std::string & error);

    static int Move(const SessionOptions & options,
        const std::string & fromPath, const std::string & toPath,
        std::string & error);

    /*
     * Versioned-object operations: S3 bucket versioning exposes version IDs,
     * version listing, version-specific retrieval and delete markers.
     * These are distinct from the current-object CRUD above: listing
     * versions includes delete markers and prior versions; getting a
     * specific version uses the ?versionId= query; deleting by version ID
     * is a permanent removal, while deleting without one creates a delete
     * marker.
     */

    /* List object versions with pagination (ListObjectVersions).
     * Each name carries its versionId. */
    static bool ListVersions(const SessionOptions & options,
        const std::string & remotePath, LscpNames * names,
        std::string & error);

    /* Download a specific version of an object by version ID. */
    static int GetVersion(const SessionOptions & options,
        const std::string & remotePath, const std::string & versionId,
        const std::string & localPath, bool preserveTime,
        unsigned long long * size, LscpProgressFn progress,
        void * progressCtx, std::string & error);

    /* Delete a specific version permanently (requires version ID).
     * Without a versionId, creates a delete marker instead. */
    static int DeleteVersion(const SessionOptions & options,
        const std::string & remotePath, const std::string & versionId,
        std::string & error);
};

} // namespace linuxscp

#endif /* LINUXSCP_S3CLIENT_H */
