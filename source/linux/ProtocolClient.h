#ifndef LINUXSCP_PROTOCOLCLIENT_H
#define LINUXSCP_PROTOCOLCLIENT_H

#include <string>
#include <vector>
#include "Session.h"
#include "SftpClient.h"

namespace linuxscp {

/*
 * The curl exit codes that say the endpoint itself is unusable (it
 * could not be reached, the transport failed, it refused us at the
 * protocol level). Anything else is the server answering, which
 * 'open' must not second-guess. Shared with S3Client.cpp, whose
 * requests are signed but whose transport is the same curl.
 */
bool IsCurlEndpointUnreachable(int code);

class ProtocolClient
{
public:
    static bool IsMultiProtocol(const std::string & protocol);

    static bool List(const SessionOptions & options, const std::string & remotePath,
        LscpNames * names, std::string & error);

    static int Stat(const SessionOptions & options, const std::string & remotePath,
        LscpAttrs * attrs, std::string & error);

    /* speedKBytesPerSecond limits the transfer rate (0 is unlimited); it
     * corresponds to the '-speed=' switch of get and put. */
    static int Get(const SessionOptions & options, const std::string & remotePath,
        const std::string & localPath, unsigned long long resumeOffset,
        bool preserveTime, unsigned long long * size, LscpProgressFn progress,
        void * progressCtx, std::string & error, unsigned speedKBytesPerSecond = 0);

    static int Put(const SessionOptions & options, const std::string & localPath,
        const std::string & remotePath, int permissions, unsigned long long resumeOffset,
        bool preserveTime, unsigned long long * size, LscpProgressFn progress,
        void * progressCtx, std::string & error, unsigned speedKBytesPerSecond = 0);

    /*
     * Ask the endpoint whether it is there at all, by listing a directory.
     * The curl-backed protocols have no connection of their own, so without
     * this an 'open' of a misspelled host or a dead port would report
     * success and every command afterwards would fail on its own. Only a
     * failure to reach or authenticate at the endpoint is reported as false:
     * a directory the server refuses to list counts as an answer.
     */
    static bool CheckEndpoint(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    static int Remove(const SessionOptions & options, const std::string & remotePath,
        std::string & error);

    static int RemoveDir(const SessionOptions & options, const std::string & remotePath,
        std::string & error);

    static int MakeDir(const SessionOptions & options, const std::string & remotePath,
        std::string & error);

    /* Server-side file copy where the protocol provides it. Returns 1 when
     * no copy operation is available so the caller can stream through the
     * client, 0 on success, and -1 on a real server/transport failure. */
    static int Copy(const SessionOptions & options, const std::string & fromPath,
        const std::string & toPath, std::string & error);

    static int Move(const SessionOptions & options, const std::string & fromPath,
        const std::string & toPath, std::string & error);

    static int SetPermissions(const SessionOptions & options,
        const std::string & remotePath, int permissions, std::string & error);

    static void FreeNames(LscpNames * names);
};

} // namespace linuxscp

#endif /* LINUXSCP_PROTOCOLCLIENT_H */
