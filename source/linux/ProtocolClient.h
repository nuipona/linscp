#ifndef LINUXSCP_PROTOCOLCLIENT_H
#define LINUXSCP_PROTOCOLCLIENT_H

#include <string>
#include <vector>
#include "Session.h"
#include "SftpClient.h"

namespace linuxscp {

class ProtocolClient
{
public:
    static bool IsMultiProtocol(const std::string & protocol);

    static bool List(const SessionOptions & options, const std::string & remotePath,
        LscpNames * names, std::string & error);

    static int Stat(const SessionOptions & options, const std::string & remotePath,
        LscpAttrs * attrs, std::string & error);

    static int Get(const SessionOptions & options, const std::string & remotePath,
        const std::string & localPath, unsigned long long resumeOffset,
        bool preserveTime, unsigned long long * size, LscpProgressFn progress,
        void * progressCtx, std::string & error);

    static int Put(const SessionOptions & options, const std::string & localPath,
        const std::string & remotePath, int permissions, unsigned long long resumeOffset,
        bool preserveTime, unsigned long long * size, LscpProgressFn progress,
        void * progressCtx, std::string & error);

    static int Remove(const SessionOptions & options, const std::string & remotePath,
        std::string & error);

    static int RemoveDir(const SessionOptions & options, const std::string & remotePath,
        std::string & error);

    static int MakeDir(const SessionOptions & options, const std::string & remotePath,
        std::string & error);

    static int Move(const SessionOptions & options, const std::string & fromPath,
        const std::string & toPath, std::string & error);

    static int SetPermissions(const SessionOptions & options,
        const std::string & remotePath, int permissions, std::string & error);

    static void FreeNames(LscpNames * names);
};

} // namespace linuxscp

#endif /* LINUXSCP_PROTOCOLCLIENT_H */
