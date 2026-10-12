#include "ProtocolClient.h"
#include "PathUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace linuxscp {

namespace {

std::string ToLower(const std::string & text)
{
    std::string result = text;
    for (size_t i = 0; i < result.size(); i++)
    {
        if (result[i] >= 'A' && result[i] <= 'Z')
        {
            result[i] = static_cast<char>(result[i] - 'A' + 'a');
        }
    }
    return result;
}

bool StartsWith(const std::string & text, const char * prefix)
{
    size_t len = strlen(prefix);
    return text.size() >= len && text.compare(0, len, prefix) == 0;
}

int RunCommand(const std::vector<std::string> & args, std::string & outStr, std::string & errStr)
{
    outStr.clear();
    errStr.clear();

    int outPipe[2];
    int errPipe[2];
    if (pipe(outPipe) != 0)
    {
        errStr = "Failed to create stdout pipe: " + std::string(strerror(errno));
        return -1;
    }
    if (pipe(errPipe) != 0)
    {
        close(outPipe[0]);
        close(outPipe[1]);
        errStr = "Failed to create stderr pipe: " + std::string(strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        errStr = "Failed to fork: " + std::string(strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        // Child process
        close(outPipe[0]);
        close(errPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[1]);
        close(errPipe[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (size_t i = 0; i < args.size(); i++)
        {
            argv.push_back(const_cast<char *>(args[i].c_str()));
        }
        argv.push_back(NULL);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent process
    close(outPipe[1]);
    close(errPipe[1]);

    // Read stdout and stderr until EOF
    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = read(outPipe[0], buffer, sizeof(buffer))) > 0)
    {
        outStr.append(buffer, bytesRead);
    }
    close(outPipe[0]);

    while ((bytesRead = read(errPipe[0], buffer, sizeof(buffer))) > 0)
    {
        errStr.append(buffer, bytesRead);
    }
    close(errPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            break;
        }
    }

    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return -1;
}

std::string BuildUrl(const SessionOptions & options, const std::string & remotePath)
{
    std::string proto = ToLower(options.protocol);
    if (proto == "webdav" || proto == "dav")
    {
        proto = "https";
    }
    else if (proto == "ssh")
    {
        proto = "scp";
    }
    else if (proto == "s3")
    {
        proto = "https";
    }

    std::string url = proto + "://" + options.hostName;
    if (options.portNumber > 0 &&
        !((proto == "ftp" && options.portNumber == 21) ||
          (proto == "ftps" && options.portNumber == 990) ||
          (proto == "http" && options.portNumber == 80) ||
          (proto == "https" && options.portNumber == 443) ||
          ((proto == "scp" || proto == "sftp") && options.portNumber == 22)))
    {
        url += ":" + std::to_string(options.portNumber);
    }

    std::string path = remotePath;
    if (path.empty() || path[0] != '/')
    {
        url += "/";
    }
    url += path;
    return url;
}

void BuildCommonCurlArgs(const SessionOptions & options, std::vector<std::string> & args)
{
    args.push_back("curl");
    args.push_back("-s");
    args.push_back("-S");

    if (!options.userName.empty())
    {
        std::string cred = options.userName;
        if (!options.password.empty())
        {
            cred += ":" + options.password;
        }
        args.push_back("-u");
        args.push_back(cred);
    }

    if (options.hostKey == "*")
    {
        args.push_back("--insecure");
    }

    if (options.timeoutSecs > 0)
    {
        args.push_back("--connect-timeout");
        args.push_back(std::to_string(options.timeoutSecs));
    }
}

} // namespace

bool ProtocolClient::IsMultiProtocol(const std::string & proto)
{
    std::string p = ToLower(proto);
    return (p == "ftp" || p == "ftps" || p == "http" || p == "https" ||
            p == "dav" || p == "webdav" || p == "s3" || p == "scp");
}

void ProtocolClient::FreeNames(LscpNames * names)
{
    if (names == NULL || names->names == NULL)
    {
        return;
    }
    for (int i = 0; i < names->count; i++)
    {
        free(names->names[i].filename);
        free(names->names[i].longname);
    }
    free(names->names);
    names->names = NULL;
    names->count = 0;
}

bool ProtocolClient::List(const SessionOptions & options, const std::string & remotePath,
    LscpNames * names, std::string & error)
{
    if (names == NULL)
    {
        error = "Null output pointer.";
        return false;
    }
    names->count = 0;
    names->names = NULL;

    std::string path = remotePath;
    if (path.empty() || path.back() != '/')
    {
        path += "/";
    }

    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    std::string proto = ToLower(options.protocol);
    if (proto == "ftp" || proto == "ftps")
    {
        args.push_back("-l"); // List-only for FTP
    }

    std::string url = BuildUrl(options, path);
    args.push_back(url);

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Curl error " + std::to_string(rc) : errStr;
        return false;
    }

    std::istringstream stream(outStr);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line == "." || line == "..")
        {
            continue;
        }
        lines.push_back(line);
    }

    names->count = static_cast<int>(lines.size());
    if (names->count == 0)
    {
        names->names = NULL;
        return true;
    }

    names->names = static_cast<LscpName *>(calloc(names->count, sizeof(LscpName)));
    if (names->names == NULL)
    {
        error = "Out of memory allocating directory entries.";
        return false;
    }

    for (int i = 0; i < names->count; i++)
    {
        std::string name = lines[i];
        bool isDir = false;
        if (!name.empty() && name.back() == '/')
        {
            name.pop_back();
            isDir = true;
        }
        names->names[i].filename = strdup(name.c_str());
        names->names[i].longname = strdup(lines[i].c_str());
        names->names[i].attrs.flags = LSCP_ATTR_PERMISSIONS;
        names->names[i].attrs.permissions = isDir ? 0040755 : 0100644;
        names->names[i].attrs.size = 0;
        names->names[i].attrs.mtime = 0;
    }

    return true;
}

int ProtocolClient::Stat(const SessionOptions & options, const std::string & remotePath,
    LscpAttrs * attrs, std::string & error)
{
    if (attrs != NULL)
    {
        memset(attrs, 0, sizeof(*attrs));
    }

    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);
    args.push_back("-I"); // HEAD request
    args.push_back(BuildUrl(options, remotePath));

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr;
        return LSCP_SFTP_STAT_MISSING;
    }

    if (attrs != NULL)
    {
        attrs->flags = LSCP_ATTR_PERMISSIONS | LSCP_ATTR_SIZE;
        attrs->permissions = 0100644;
        attrs->size = 0;
        std::istringstream stream(outStr);
        std::string line;
        while (std::getline(stream, line))
        {
            if (StartsWith(ToLower(line), "content-length:"))
            {
                size_t pos = line.find(':');
                if (pos != std::string::npos)
                {
                    try {
                        attrs->size = std::stoull(line.substr(pos + 1));
                    } catch (...) {}
                }
            }
        }
    }
    return LSCP_SFTP_STAT_EXISTS;
}

int ProtocolClient::Get(const SessionOptions & options, const std::string & remotePath,
    const std::string & localPath, unsigned long long resumeOffset,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx,
    std::string & error)
{
    if (size != NULL)
    {
        *size = 0;
    }

    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    if (resumeOffset > 0)
    {
        args.push_back("-C");
        args.push_back(std::to_string(resumeOffset));
    }

    args.push_back("-o");
    args.push_back(localPath);
    args.push_back(BuildUrl(options, remotePath));

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Download failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }

    struct stat st;
    if (stat(localPath.c_str(), &st) == 0 && size != NULL)
    {
        *size = static_cast<unsigned long long>(st.st_size);
    }
    return 0;
}

int ProtocolClient::Put(const SessionOptions & options, const std::string & localPath,
    const std::string & remotePath, int permissions, unsigned long long resumeOffset,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx,
    std::string & error)
{
    if (size != NULL)
    {
        *size = 0;
    }

    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    if (resumeOffset > 0)
    {
        args.push_back("-C");
        args.push_back(std::to_string(resumeOffset));
    }

    args.push_back("-T");
    args.push_back(localPath);
    args.push_back(BuildUrl(options, remotePath));

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Upload failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }

    struct stat st;
    if (stat(localPath.c_str(), &st) == 0 && size != NULL)
    {
        *size = static_cast<unsigned long long>(st.st_size);
    }
    return 0;
}

int ProtocolClient::Remove(const SessionOptions & options, const std::string & remotePath,
    std::string & error)
{
    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    std::string proto = ToLower(options.protocol);
    if (proto == "ftp" || proto == "ftps")
    {
        args.push_back("-Q");
        args.push_back("DELE " + remotePath);
        args.push_back(BuildUrl(options, "/"));
    }
    else
    {
        args.push_back("-X");
        args.push_back("DELETE");
        args.push_back(BuildUrl(options, remotePath));
    }

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Remove failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }
    return 0;
}

int ProtocolClient::RemoveDir(const SessionOptions & options, const std::string & remotePath,
    std::string & error)
{
    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    std::string proto = ToLower(options.protocol);
    if (proto == "ftp" || proto == "ftps")
    {
        args.push_back("-Q");
        args.push_back("RMD " + remotePath);
        args.push_back(BuildUrl(options, "/"));
    }
    else
    {
        args.push_back("-X");
        args.push_back("DELETE");
        args.push_back(BuildUrl(options, remotePath));
    }

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Remove directory failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }
    return 0;
}

int ProtocolClient::MakeDir(const SessionOptions & options, const std::string & remotePath,
    std::string & error)
{
    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    std::string proto = ToLower(options.protocol);
    if (proto == "ftp" || proto == "ftps")
    {
        args.push_back("-Q");
        args.push_back("MKD " + remotePath);
        args.push_back(BuildUrl(options, "/"));
    }
    else
    {
        args.push_back("-X");
        args.push_back("MKCOL");
        args.push_back(BuildUrl(options, remotePath));
    }

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Create directory failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }
    return 0;
}

int ProtocolClient::Move(const SessionOptions & options, const std::string & fromPath,
    const std::string & toPath, std::string & error)
{
    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    std::string proto = ToLower(options.protocol);
    if (proto == "ftp" || proto == "ftps")
    {
        args.push_back("-Q");
        args.push_back("RNFR " + fromPath);
        args.push_back("-Q");
        args.push_back("RNTO " + toPath);
        args.push_back(BuildUrl(options, "/"));
    }
    else
    {
        args.push_back("-X");
        args.push_back("MOVE");
        args.push_back("-H");
        args.push_back("Destination: " + toPath);
        args.push_back(BuildUrl(options, fromPath));
    }

    std::string outStr;
    std::string errStr;
    int rc = RunCommand(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Move failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }
    return 0;
}

} // namespace linuxscp
