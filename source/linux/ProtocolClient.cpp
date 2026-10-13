#include "ProtocolClient.h"
#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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

char * DupString(const std::string & text)
{
    char * copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy != NULL)
    {
        memcpy(copy, text.data(), text.size());
        copy[text.size()] = '\0';
    }
    return copy;
}

/* curl is the transfer engine for every non-PuTTY protocol. On Windows
 * it is the curl.exe that ships with the system (Windows 10 1803+);
  * FindExecutableOnPath applies PATHEXT there and PATH elsewhere. */
const std::string & CurlExecutable()
{
    static const std::string executable = [] {
        std::string found = FindExecutableOnPath("curl");
        return found.empty() ? std::string("curl") : found;
    }();
    return executable;
}

int RunCurl(const std::vector<std::string> & args, std::string & outStr,
    std::string & errStr)
{
    int rc = RunCapture(args, outStr, errStr);
    if (rc == -1 && errStr.empty())
    {
        errStr = "Failed to run curl. Install curl (on Windows 10/11 it "
            "ships as curl.exe) and make sure it is on PATH.";
    }
    else if (rc == 127 && FindExecutableOnPath("curl").empty())
    {
        errStr = "curl was not found on PATH. Install curl (on Windows "
            "10/11 it ships as curl.exe) to use this protocol.";
    }
    return rc;
}

/* Splits an 'ls -l'-style line into exactly 9 fields (the 9th keeps any
 * spaces of the file name). Returns false for bare-name listings. */
bool SplitLongListing(const std::string & line, std::vector<std::string> & fields)
{
    fields.clear();
    std::istringstream stream(line);
    std::string field;
    while (fields.size() < 8 && (stream >> field))
    {
        fields.push_back(field);
    }
    if (fields.size() < 8)
    {
        return false;
    }
    std::string rest;
    std::getline(stream, rest);
    size_t first = rest.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return false;
    }
    fields.push_back(rest.substr(first));
    return true;
}

bool LooksLikePermissions(const std::string & text)
{
    if (text.size() != 10)
    {
        return false;
    }
    const char types[] = "-dlbcps";
    if (strchr(types, text[0]) == NULL)
    {
        return false;
    }
    for (size_t i = 1; i < 10; i++)
    {
        if (strchr("rwxstST-", text[i]) == NULL)
        {
            return false;
        }
    }
    return true;
}

unsigned long PermissionsFromString(const std::string & text)
{
    unsigned long mode = text[0] == 'd' ? 0040000UL : 0100000UL;
    static const unsigned long bits[9] = {
        0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001
    };
    for (size_t i = 0; i < 9 && i + 1 < text.size(); i++)
    {
        char c = text[i + 1];
        if (c == 'r' || c == 'w' || c == 'x' || c == 's' || c == 't')
        {
            mode |= bits[i];
        }
    }
    return mode;
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

bool IsSshCurlProtocol(const std::string & proto)
{
    return proto == "sftp" || proto == "scp";
}

void BuildCommonCurlArgs(const SessionOptions & options, std::vector<std::string> & args)
{
    args.push_back(CurlExecutable());
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
    else if (!options.hostKey.empty() && IsSshCurlProtocol(ToLower(options.protocol)))
    {
        /* curl pins SSH host keys by MD5 fingerprint (--hostpubmd5).
         * A SHA256 pin cannot be expressed for curl; host verification
         * then falls back to the OpenSSH known_hosts file, exactly as
         * with the system ssh/sftp clients. */
        std::string md5;
        for (size_t i = 0; i < options.hostKey.size(); i++)
        {
            if (options.hostKey[i] != ':')
            {
                md5 += options.hostKey[i];
            }
        }
        bool hex = md5.size() == 32;
        for (size_t i = 0; hex && i < md5.size(); i++)
        {
            char c = md5[i];
            hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F');
        }
        if (hex)
        {
            args.push_back("--hostpubmd5");
            args.push_back(md5);
        }
    }

    if (!options.privateKeyFile.empty())
    {
        args.push_back("--key");
        args.push_back(ExpandTilde(options.privateKeyFile));
    }

    if (options.timeoutSecs > 0)
    {
        args.push_back("--connect-timeout");
        args.push_back(std::to_string(options.timeoutSecs));
    }
}

std::string OctalPermissions(int permissions)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%o", static_cast<unsigned int>(permissions));
    return std::string(buffer);
}

} // namespace

bool ProtocolClient::IsMultiProtocol(const std::string & proto)
{
    std::string p = ToLower(proto);
    if (p == "ftp" || p == "ftps" || p == "http" || p == "https" ||
        p == "dav" || p == "webdav" || p == "s3" || p == "scp")
    {
        return true;
    }
    /* SFTP is served by the PuTTY engine where that exists, and by
     * curl everywhere else (Windows). */
    return p == "sftp" && !kHasNativeSftp;
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
    int rc = RunCurl(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Curl error " + std::to_string(rc) : errStr;
        return false;
    }

    struct Entry
    {
        std::string name;
        std::string longname;
        bool isDir;
        unsigned long permissions;
        unsigned long long size;
    };
    std::vector<Entry> entries;

    std::istringstream stream(outStr);
    std::string line;
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

        Entry entry;
        entry.isDir = false;
        entry.permissions = 0100644;
        entry.size = 0;
        entry.longname = line;

        std::vector<std::string> fields;
        if (SplitLongListing(line, fields) && LooksLikePermissions(fields[0]))
        {
            /* 'ls -l' style output (SFTP and some FTP servers). */
            entry.name = fields[8];
            entry.isDir = fields[0][0] == 'd';
            entry.permissions = PermissionsFromString(fields[0]);
            try
            {
                entry.size = std::stoull(fields[4]);
            }
            catch (...)
            {
                entry.size = 0;
            }
        }
        else
        {
            entry.name = line;
            if (!entry.name.empty() && entry.name.back() == '/')
            {
                entry.name.pop_back();
                entry.isDir = true;
                entry.permissions = 0040755;
            }
        }
        if (entry.name.empty() || entry.name == "." || entry.name == "..")
        {
            continue;
        }
        entries.push_back(entry);
    }

    names->count = static_cast<int>(entries.size());
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
        names->names[i].filename = DupString(entries[i].name);
        names->names[i].longname = DupString(entries[i].longname);
        names->names[i].attrs.flags = LSCP_ATTR_PERMISSIONS | LSCP_ATTR_SIZE;
        names->names[i].attrs.permissions = entries[i].permissions;
        names->names[i].attrs.size = entries[i].size;
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

    std::string proto = ToLower(options.protocol);
    if (proto == "sftp")
    {
        /* curl cannot HEAD an SFTP path; stat by listing the parent. */
        std::string fileName = RemoteFileName(remotePath);
        if (fileName.empty())
        {
            if (attrs != NULL)
            {
                attrs->flags = LSCP_ATTR_PERMISSIONS;
                attrs->permissions = 0040755;
            }
            return LSCP_SFTP_STAT_EXISTS;
        }
        std::string parent = RemoteDirName(remotePath);
        if (parent.empty())
        {
            parent = "/";
        }
        LscpNames names;
        names.count = 0;
        names.names = NULL;
        if (!List(options, parent, &names, error))
        {
            return LSCP_SFTP_STAT_ERROR;
        }
        int result = LSCP_SFTP_STAT_MISSING;
        for (int i = 0; i < names.count; i++)
        {
            if (names.names[i].filename != NULL &&
                fileName == names.names[i].filename)
            {
                if (attrs != NULL)
                {
                    *attrs = names.names[i].attrs;
                }
                result = LSCP_SFTP_STAT_EXISTS;
                break;
            }
        }
        FreeNames(&names);
        if (result == LSCP_SFTP_STAT_MISSING)
        {
            error = "No such file or directory.";
        }
        return result;
    }

    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);
    args.push_back("-I"); // HEAD request
    args.push_back(BuildUrl(options, remotePath));

    std::string outStr;
    std::string errStr;
    int rc = RunCurl(args, outStr, errStr);
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
    (void)progress;
    (void)progressCtx;
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
    int rc = RunCurl(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Download failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }

    LocalFileInfo info = StatLocal(localPath);
    if (info.exists && size != NULL)
    {
        *size = info.size;
    }
    return 0;
}

int ProtocolClient::Put(const SessionOptions & options, const std::string & localPath,
    const std::string & remotePath, int permissions, unsigned long long resumeOffset,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx,
    std::string & error)
{
    (void)permissions;
    (void)progress;
    (void)progressCtx;
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
    int rc = RunCurl(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Upload failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }

    LocalFileInfo info = StatLocal(localPath);
    if (info.exists && size != NULL)
    {
        *size = info.size;
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
    else if (IsSshCurlProtocol(proto))
    {
        args.push_back("-Q");
        args.push_back("rm " + remotePath);
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
    int rc = RunCurl(args, outStr, errStr);
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
    else if (IsSshCurlProtocol(proto))
    {
        args.push_back("-Q");
        args.push_back("rmdir " + remotePath);
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
    int rc = RunCurl(args, outStr, errStr);
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
    else if (IsSshCurlProtocol(proto))
    {
        args.push_back("-Q");
        args.push_back("mkdir " + remotePath);
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
    int rc = RunCurl(args, outStr, errStr);
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
    else if (IsSshCurlProtocol(proto))
    {
        args.push_back("-Q");
        args.push_back("rename " + fromPath + " " + toPath);
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
    int rc = RunCurl(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Move failed with exit code " + std::to_string(rc) : errStr;
        return -1;
    }
    return 0;
}

int ProtocolClient::SetPermissions(const SessionOptions & options,
    const std::string & remotePath, int permissions, std::string & error)
{
    std::vector<std::string> args;
    BuildCommonCurlArgs(options, args);

    std::string proto = ToLower(options.protocol);
    if (proto == "ftp" || proto == "ftps")
    {
        args.push_back("-Q");
        args.push_back("SITE CHMOD " + OctalPermissions(permissions) + " " + remotePath);
        args.push_back(BuildUrl(options, "/"));
    }
    else if (proto == "sftp")
    {
        args.push_back("-Q");
        args.push_back("chmod " + OctalPermissions(permissions) + " " + remotePath);
        args.push_back(BuildUrl(options, "/"));
    }
    else
    {
        error = "chmod is not supported by the " + options.protocol + " protocol.";
        return -1;
    }

    std::string outStr;
    std::string errStr;
    int rc = RunCurl(args, outStr, errStr);
    if (rc != 0)
    {
        error = errStr.empty() ? "Change permissions failed with exit code " +
            std::to_string(rc) : errStr;
        return -1;
    }
    return 0;
}

} // namespace linuxscp
