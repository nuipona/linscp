#include "Script.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>

#include "Console.h"
#include "LocalFs.h"
#include "PathUtil.h"
#include "Texts.h"

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

void ProgressCallback(unsigned long long transferred, unsigned long long total, void * ctx)
{
    (void)total;
    (void)ctx;
    Console::Instance().TransferProgress(transferred);
}

bool StartsWith(const std::string & text, const char * prefix)
{
    size_t len = strlen(prefix);
    return text.size() > len && text.compare(0, len, prefix) == 0;
}

enum class ConfirmationResult
{
    Accepted,
    Declined,
    Unavailable
};

ConfirmationResult ConfirmDestructiveAction(const std::string & question)
{
    Console & console = Console::Instance();
    if (!console.IsInteractive())
    {
        console.Error("Cannot confirm this action without a terminal; run "
            "interactively or use 'option confirm off' in a trusted script.");
        return ConfirmationResult::Unavailable;
    }
    if (console.Confirm(question, false))
    {
        return ConfirmationResult::Accepted;
    }
    console.Status("Cancelled.");
    return ConfirmationResult::Declined;
}

bool ParseOctalPermissions(const std::string & text, unsigned long & permissions)
{
    if (text.empty())
    {
        return false;
    }
    unsigned long value = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] < '0' || text[i] > '7')
        {
            return false;
        }
        value = value * 8 + static_cast<unsigned long>(text[i] - '0');
        if (value > 07777)
        {
            return false;
        }
    }
    permissions = value;
    return true;
}

} // namespace

Script::Script() :
    mConfirm(true),
    mExit(false),
    mErrors(0)
{
}

bool Script::RequireConnection()
{
    if (!mSession.IsOpen() || !mSession.IsAlive())
    {
        if (mSession.IsOpen())
        {
            mSession.Close();
            mRemoteDirectory.clear();
        }
        Console::Instance().Error(Texts::NotConnected());
        mErrors++;
        return false;
    }
    return true;
}

bool Script::OpenSite(const std::vector<std::string> & args)
{
    /* Command-line options are defaults for every opened site. Explicit
     * values in the URL or on this command override them; do not reset the
     * port or credentials here (doing so made -port/-username/-password
     * silently ineffective). */
    SessionOptions options = mDefaults;
    options.hostName.clear();
    options.initialDirectory.clear();

    std::string url;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (StartsWith(args[i], "-hostkey="))
        {
            options.hostKey = args[i].substr(strlen("-hostkey="));
        }
        else if (StartsWith(args[i], "-password="))
        {
            options.password = args[i].substr(strlen("-password="));
        }
        else if (StartsWith(args[i], "-username="))
        {
            options.userName = args[i].substr(strlen("-username="));
        }
        else if (StartsWith(args[i], "-privatekey="))
        {
            options.privateKeyFile = args[i].substr(strlen("-privatekey="));
        }
        else if (StartsWith(args[i], "-"))
        {
            Console::Instance().Error(Texts::WrongArguments("open",
                "Unknown switch '" + args[i] + "'."));
            mErrors++;
            return false;
        }
        else if (url.empty())
        {
            url = args[i];
        }
        else
        {
            Console::Instance().Error(Texts::WrongArguments("open",
                "Unexpected argument '" + args[i] + "'."));
            mErrors++;
            return false;
        }
    }

    if (url.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("open",
            "Syntax: open sftp://[user[:password]@]host[:port]/[path]"));
        mErrors++;
        return false;
    }

    std::string error;
    if (!ParseSiteUrl(url, options, error))
    {
        Console::Instance().Error(error);
        mErrors++;
        return false;
    }
    if (options.userName.empty())
    {
        const char * user = getenv("USER");
        if (user != NULL)
        {
            options.userName = user;
        }
    }

    Console::Instance().Status("Connecting to " + options.hostName + "...");
    if (!mSession.Open(options, error))
    {
        Console::Instance().Error(Texts::CannotOpenSession(options.hostName, error));
        mErrors++;
        return false;
    }

    if (lscp_sftp_init() != 0)
    {
        Console::Instance().Error(lscp_sftp_error());
        mSession.Close();
        mErrors++;
        return false;
    }

    std::string initial = options.initialDirectory.empty() ?
        std::string(".") : options.initialDirectory;
    char * canonical = lscp_sftp_realpath(initial.c_str());
    if (canonical == NULL)
    {
        Console::Instance().Error(lscp_sftp_error());
        mSession.Close();
        mErrors++;
        return false;
    }
    mRemoteDirectory = canonical;
    free(canonical);

    Console::Instance().Status("Connected, the remote directory is " + mRemoteDirectory);
    return true;
}

void Script::CloseSite()
{
    if (!mSession.IsOpen())
    {
        Console::Instance().Error(Texts::NotConnected());
        mErrors++;
        return;
    }
    mSession.Close();
    mRemoteDirectory.clear();
}

bool Script::ChangeRemoteDirectory(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 1)
    {
        Console::Instance().Error(Texts::WrongArguments("cd", "Syntax: cd <directory>"));
        mErrors++;
        return false;
    }
    std::string path = ResolveRemotePath(mRemoteDirectory, args[0]);
    char * canonical = lscp_sftp_realpath(path.c_str());
    if (canonical == NULL)
    {
        Console::Instance().Error(Texts::RemoteError("change directory to", path,
            lscp_sftp_error()));
        mErrors++;
        return false;
    }
    mRemoteDirectory = canonical;
    free(canonical);
    return true;
}

void Script::PrintRemoteDirectory()
{
    if (!RequireConnection())
    {
        return;
    }
    Console::Instance().WriteLine(mRemoteDirectory);
}

void Script::PrintRemoteEntry(const std::string & name, const LscpName & entry,
    bool longFormat)
{
    if (!longFormat)
    {
        Console::Instance().WriteLine(name);
        return;
    }
    if (entry.longname != NULL && entry.longname[0] != '\0')
    {
        Console::Instance().WriteLine(entry.longname);
        return;
    }
    std::string line = FormatPermissions(entry.attrs.permissions);
    line += "  1";
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "  %-8lu %-8lu %12llu",
        entry.attrs.uid, entry.attrs.gid, entry.attrs.size);
    line += buffer;
    line += " " + FormatFileTime(entry.attrs.mtime) + " " + name;
    Console::Instance().WriteLine(line);
}

bool Script::ListRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    bool longFormat = true;
    std::string target;
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & arg = args[i];
        if (arg.size() > 1 && arg[0] == '-')
        {
            for (size_t j = 1; j < arg.size(); j++)
            {
                switch (arg[j])
                {
                case 'a': /* entries starting with a dot are always listed */
                    break;
                case 'l':
                    longFormat = true;
                    break;
                case '1':
                    longFormat = false;
                    break;
                default:
                    Console::Instance().Error(Texts::WrongArguments("ls",
                        "Syntax: ls [-al1] [<directory>]"));
                    mErrors++;
                    return false;
                }
            }
        }
        else if (target.empty())
        {
            target = arg;
        }
        else
        {
            Console::Instance().Error(Texts::WrongArguments("ls",
                "Syntax: ls [-al1] [<directory>]"));
            mErrors++;
            return false;
        }
    }
    std::string path = target.empty() ? mRemoteDirectory :
        ResolveRemotePath(mRemoteDirectory, target);
    Console::Instance().Status(Texts::ReadingDirectory(path));
    LscpNames * names = lscp_sftp_list_dir(path.c_str());
    if (names == NULL)
    {
        Console::Instance().Error(Texts::CannotReadDirectory(path, lscp_sftp_error()));
        mErrors++;
        return false;
    }
    int count = 0;
    for (int i = 0; i < names->count; i++)
    {
        std::string name = names->names[i].filename != NULL ?
            names->names[i].filename : "";
        if (name == "." || name == "..")
        {
            continue;
        }
        PrintRemoteEntry(name, names->names[i], longFormat);
        count++;
    }
    lscp_sftp_free_names(names);
    Console::Instance().Status(Texts::DirectoryRead(count));
    return true;
}

bool Script::ChangeLocalDirectory(const std::vector<std::string> & args)
{
    std::string error;
    if (args.size() != 1)
    {
        Console::Instance().Error(Texts::WrongArguments("lcd", "Syntax: lcd <directory>"));
        mErrors++;
        return false;
    }
    std::string current = GetLocalDirectory(error);
    if (current.empty())
    {
        Console::Instance().Error(error);
        mErrors++;
        return false;
    }
    std::string path = ResolveLocalPath(current, args[0]);
    if (!SetLocalDirectory(path, error))
    {
        Console::Instance().Error(Texts::LocalError("change of directory to " + path,
            path, error));
        mErrors++;
        return false;
    }
    return true;
}

void Script::PrintLocalDirectory()
{
    std::string error;
    std::string current = GetLocalDirectory(error);
    if (current.empty())
    {
        Console::Instance().Error(error);
        mErrors++;
        return;
    }
    Console::Instance().WriteLine(current);
}

bool Script::ExpandRemote(const std::string & pattern, std::vector<std::string> & paths,
    std::string & error)
{
    paths.clear();
    if (!HasWildcard(pattern))
    {
        paths.push_back(ResolveRemotePath(mRemoteDirectory, pattern));
        return true;
    }
    std::string absolute = ResolveRemotePath(mRemoteDirectory, pattern);
    std::string dir = RemoteDirName(absolute);
    std::string mask = RemoteFileName(absolute);
    if (dir.empty())
    {
        dir = "/";
    }
    LscpNames * names = lscp_sftp_list_dir(dir.c_str());
    if (names == NULL)
    {
        error = lscp_sftp_error();
        return false;
    }
    for (int i = 0; i < names->count; i++)
    {
        std::string name = names->names[i].filename != NULL ? names->names[i].filename : "";
        if (!MatchesMask(mask, name))
        {
            continue;
        }
        if (name == "." || name == "..")
        {
            continue;
        }
        paths.push_back(JoinRemotePath(dir, name));
    }
    lscp_sftp_free_names(names);
    return true;
}

bool Script::CreateRemoteDirectoryRecursive(const std::string & path, std::string & error)
{
    error.clear();
    if (path.empty() || path == "/")
    {
        return true;
    }
    LscpAttrs attrs = {};
    int st = lscp_sftp_stat(path.c_str(), &attrs);
    if (st == LSCP_SFTP_STAT_EXISTS)
    {
        if ((attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 && (attrs.permissions & 0040000) != 0)
        {
            return true;
        }
        error = "Path '" + path + "' exists and is not a directory.";
        return false;
    }
    std::string parent = RemoteDirName(path);
    if (!parent.empty() && parent != path && parent != "/")
    {
        if (!CreateRemoteDirectoryRecursive(parent, error))
        {
            return false;
        }
    }
    if (lscp_sftp_mkdir(path.c_str(), -1) != 0)
    {
        error = lscp_sftp_error();
        return false;
    }
    return true;
}

bool Script::DownloadDirectoryRecursive(const std::string & remoteDir,
    const std::string & localDir, const TransferFlags & flags,
    unsigned long long & bytesTransferred)
{
    std::string error;
    if (!CreateLocalDirectoryRecursive(localDir, error))
    {
        Console::Instance().Error(Texts::LocalError("create directory", localDir, error));
        mErrors++;
        return false;
    }

    LscpNames * names = lscp_sftp_list_dir(remoteDir.c_str());
    if (names == NULL)
    {
        Console::Instance().Error(Texts::RemoteError("read directory", remoteDir, lscp_sftp_error()));
        mErrors++;
        return false;
    }

    bool success = true;
    for (int i = 0; i < names->count; i++)
    {
        const char * rawName = names->names[i].filename;
        if (rawName == NULL)
        {
            continue;
        }
        std::string name(rawName);
        if (name == "." || name == "..")
        {
            continue;
        }

        std::string childRemote = JoinRemotePath(remoteDir, name);
        std::string childLocal = JoinLocalPath(localDir, name);

        bool childIsDir = (names->names[i].attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
            (names->names[i].attrs.permissions & 0040000) != 0;

        if (!childIsDir && (names->names[i].attrs.flags & LSCP_ATTR_PERMISSIONS) == 0)
        {
            LscpAttrs a = {};
            if (lscp_sftp_stat(childRemote.c_str(), &a) == LSCP_SFTP_STAT_EXISTS)
            {
                childIsDir = (a.flags & LSCP_ATTR_PERMISSIONS) != 0 && (a.permissions & 0040000) != 0;
            }
        }

        if (childIsDir)
        {
            if (!DownloadDirectoryRecursive(childRemote, childLocal, flags, bytesTransferred))
            {
                success = false;
            }
            if (flags.deleteSource)
            {
                lscp_sftp_rmdir(childRemote.c_str());
            }
        }
        else
        {
            LocalFileInfo localInfo = StatLocal(childLocal);
            if (flags.newerOnly && localInfo.exists &&
                (names->names[i].attrs.flags & LSCP_ATTR_ACMODTIME) != 0 &&
                localInfo.mtime >= names->names[i].attrs.mtime)
            {
                continue;
            }
            if (mConfirm && localInfo.exists)
            {
                ConfirmationResult confirmation = ConfirmDestructiveAction(
                    "Overwrite local file '" + childLocal + "'?");
                if (confirmation != ConfirmationResult::Accepted)
                {
                    if (confirmation == ConfirmationResult::Unavailable)
                    {
                        mErrors++;
                    }
                    continue;
                }
            }

            unsigned long long size = 0;
            unsigned long long totalSize = (names->names[i].attrs.flags & LSCP_ATTR_SIZE) != 0 ?
                names->names[i].attrs.size : 0;
            Console::Instance().TransferStart(name, totalSize);
            int result = lscp_sftp_get(childRemote.c_str(), childLocal.c_str(), &size,
                ProgressCallback, NULL);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(childRemote, lscp_sftp_error()));
                mErrors++;
                success = false;
                continue;
            }
            bytesTransferred += size;
            Console::Instance().Status(Texts::FileTransferred(childLocal, size));
            if (flags.preserveTime && (names->names[i].attrs.flags & LSCP_ATTR_ACMODTIME) != 0)
            {
                struct timeval tv[2] = {
                    { static_cast<time_t>(names->names[i].attrs.atime), 0 },
                    { static_cast<time_t>(names->names[i].attrs.mtime), 0 }
                };
                utimes(childLocal.c_str(), tv);
            }
            if (flags.deleteSource)
            {
                lscp_sftp_remove(childRemote.c_str());
            }
        }
    }
    lscp_sftp_free_names(names);
    return success;
}

bool Script::UploadDirectoryRecursive(const std::string & localDir,
    const std::string & remoteDir, const TransferFlags & flags,
    unsigned long long & bytesTransferred)
{
    std::string error;
    if (!CreateRemoteDirectoryRecursive(remoteDir, error))
    {
        Console::Instance().Error(Texts::RemoteError("create directory", remoteDir, error));
        mErrors++;
        return false;
    }

    std::vector<std::string> entries;
    if (!ListLocalDirectory(localDir, entries, error))
    {
        Console::Instance().Error(Texts::LocalError("read directory", localDir, error));
        mErrors++;
        return false;
    }

    bool success = true;
    for (size_t i = 0; i < entries.size(); i++)
    {
        const std::string & name = entries[i];
        std::string childLocal = JoinLocalPath(localDir, name);
        std::string childRemote = JoinRemotePath(remoteDir, name);

        LocalFileInfo info = StatLocal(childLocal);
        if (info.isDir)
        {
            if (!UploadDirectoryRecursive(childLocal, childRemote, flags, bytesTransferred))
            {
                success = false;
            }
            if (flags.deleteSource)
            {
                rmdir(childLocal.c_str());
            }
        }
        else if (info.isRegular)
        {
            LscpAttrs remoteInfo = {};
            const int remoteStat = lscp_sftp_stat(childRemote.c_str(), &remoteInfo);
            const bool remoteExists = remoteStat == LSCP_SFTP_STAT_EXISTS;

            if (flags.newerOnly && remoteExists &&
                (remoteInfo.flags & LSCP_ATTR_ACMODTIME) != 0 &&
                remoteInfo.mtime >= info.mtime)
            {
                continue;
            }
            if (mConfirm && remoteExists)
            {
                ConfirmationResult confirmation = ConfirmDestructiveAction(
                    "Overwrite remote file '" + childRemote + "'?");
                if (confirmation != ConfirmationResult::Accepted)
                {
                    if (confirmation == ConfirmationResult::Unavailable)
                    {
                        mErrors++;
                    }
                    continue;
                }
            }

            unsigned long long size = 0;
            Console::Instance().TransferStart(name, info.size);
            int result = lscp_sftp_put(childLocal.c_str(), childRemote.c_str(), -1,
                &size, ProgressCallback, NULL);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(childLocal, lscp_sftp_error()));
                mErrors++;
                success = false;
                continue;
            }
            bytesTransferred += size;
            Console::Instance().Status(Texts::FileTransferred(childLocal, size));
            if (flags.deleteSource)
            {
                RemoveLocalFile(childLocal, error);
            }
        }
    }
    return success;
}

bool Script::GetFiles(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }

    TransferFlags flags;
    std::vector<std::string> paths;
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & a = args[i];
        if (a == "-delete")
        {
            flags.deleteSource = true;
        }
        else if (a == "-neweronly")
        {
            flags.newerOnly = true;
        }
        else if (a == "-preservetime")
        {
            flags.preserveTime = true;
        }
        else if (a == "-nopreservetime")
        {
            flags.preserveTime = false;
        }
        else if (a == "-r")
        {
            flags.recursive = true;
        }
        else if (StartsWith(a, "-filemask=") || StartsWith(a, "-transfer=") ||
                 StartsWith(a, "-resume") || a == "-resume")
        {
            /* Accepted WinSCP switch compatibility */
        }
        else
        {
            paths.push_back(a);
        }
    }

    if (paths.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("get",
            "Syntax: get [switches] <remotefile(s)> [<localpath>]"));
        mErrors++;
        return false;
    }

    std::string destination = paths.size() >= 2 ? paths.back() : ".";
    std::vector<std::string> sourcePatterns = paths.size() >= 2 ?
        std::vector<std::string>(paths.begin(), paths.end() - 1) :
        std::vector<std::string>(paths.begin(), paths.end());

    LocalFileInfo destinationInfo = StatLocal(ExpandTilde(destination));
    bool destinationIsDir = destinationInfo.isDir;

    bool any = false;
    for (size_t i = 0; i < sourcePatterns.size(); i++)
    {
        std::vector<std::string> sources;
        std::string error;
        if (!ExpandRemote(sourcePatterns[i], sources, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", sourcePatterns[i], error));
            mErrors++;
            continue;
        }
        if (sources.empty())
        {
            Console::Instance().Error(Texts::RemoteError("find", sourcePatterns[i],
                "No such file."));
            mErrors++;
            continue;
        }
        if (sources.size() > 1 || sourcePatterns.size() > 1)
        {
            if (destinationInfo.exists && !destinationInfo.isDir)
            {
                Console::Instance().Error(Texts::LocalError("download to", destination,
                    "A directory is required for multiple source files."));
                mErrors++;
                continue;
            }
            if (!destinationIsDir)
            {
                if (!CreateLocalDirectoryRecursive(ExpandTilde(destination), error))
                {
                    Console::Instance().Error(Texts::LocalError("create directory",
                        destination, error));
                    mErrors++;
                    continue;
                }
                destinationIsDir = true;
                destinationInfo = StatLocal(ExpandTilde(destination));
            }
        }
        for (size_t n = 0; n < sources.size(); n++)
        {
            LscpAttrs sourceAttrs = {};
            int statRes = lscp_sftp_stat(sources[n].c_str(), &sourceAttrs);
            bool isDir = (statRes == LSCP_SFTP_STAT_EXISTS) &&
                (sourceAttrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
                (sourceAttrs.permissions & 0040000) != 0;

            if (isDir)
            {
                if (!flags.recursive)
                {
                    Console::Instance().Error(Texts::TransferFailed(sources[n],
                        "Directories are not supported without recursive mode."));
                    mErrors++;
                    continue;
                }
                std::string targetLocalDir = (destinationIsDir || sources.size() > 1 || sourcePatterns.size() > 1) ?
                    JoinLocalPath(ExpandTilde(destination), RemoteFileName(sources[n])) :
                    ExpandTilde(destination);

                unsigned long long dirBytes = 0;
                if (DownloadDirectoryRecursive(sources[n], targetLocalDir, flags, dirBytes))
                {
                    any = true;
                    if (flags.deleteSource)
                    {
                        lscp_sftp_rmdir(sources[n].c_str());
                    }
                }
                continue;
            }

            std::string localPath;
            if (destinationIsDir || sources.size() > 1 || sourcePatterns.size() > 1)
            {
                localPath = JoinLocalPath(ExpandTilde(destination),
                    RemoteFileName(sources[n]));
            }
            else
            {
                localPath = ExpandTilde(destination);
            }

            LocalFileInfo localInfo = StatLocal(localPath);
            if (!localInfo.exists && localInfo.error != ENOENT &&
                localInfo.error != ENOTDIR)
            {
                errno = localInfo.error;
                Console::Instance().Error(Texts::LocalError("inspect destination",
                    localPath, LastSystemError()));
                mErrors++;
                continue;
            }
            if (localInfo.isDir)
            {
                Console::Instance().Error(Texts::LocalError("download to", localPath,
                    "The destination is a directory."));
                mErrors++;
                continue;
            }
            if (localInfo.exists && !localInfo.isRegular)
            {
                Console::Instance().Error(Texts::LocalError("download to", localPath,
                    "The destination is not a regular file."));
                mErrors++;
                continue;
            }
            if (flags.newerOnly && localInfo.exists &&
                (sourceAttrs.flags & LSCP_ATTR_ACMODTIME) != 0 &&
                localInfo.mtime >= sourceAttrs.mtime)
            {
                continue;
            }
            if (mConfirm && localInfo.exists)
            {
                ConfirmationResult confirmation = ConfirmDestructiveAction(
                    "Overwrite local file '" + localPath + "'?");
                if (confirmation != ConfirmationResult::Accepted)
                {
                    if (confirmation == ConfirmationResult::Unavailable)
                    {
                        mErrors++;
                    }
                    continue;
                }
            }

            unsigned long long totalSize = 0;
            if ((sourceAttrs.flags & LSCP_ATTR_SIZE) != 0)
            {
                totalSize = sourceAttrs.size;
            }
            unsigned long long size = 0;
            Console::Instance().TransferStart(RemoteFileName(sources[n]), totalSize);
            int result = lscp_sftp_get(sources[n].c_str(), localPath.c_str(), &size,
                ProgressCallback, NULL);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(sources[n],
                    lscp_sftp_error()));
                mErrors++;
                continue;
            }
            any = true;
            Console::Instance().Status(Texts::FileTransferred(localPath, size));
            if (flags.preserveTime && (sourceAttrs.flags & LSCP_ATTR_ACMODTIME) != 0)
            {
                struct timeval tv[2] = {
                    { static_cast<time_t>(sourceAttrs.atime), 0 },
                    { static_cast<time_t>(sourceAttrs.mtime), 0 }
                };
                utimes(localPath.c_str(), tv);
            }
            if (flags.deleteSource)
            {
                lscp_sftp_remove(sources[n].c_str());
            }
        }
    }
    return any;
}

bool Script::PutFiles(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }

    TransferFlags flags;
    std::vector<std::string> paths;
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & a = args[i];
        if (a == "-delete")
        {
            flags.deleteSource = true;
        }
        else if (a == "-neweronly")
        {
            flags.newerOnly = true;
        }
        else if (a == "-preservetime")
        {
            flags.preserveTime = true;
        }
        else if (a == "-nopreservetime")
        {
            flags.preserveTime = false;
        }
        else if (a == "-r")
        {
            flags.recursive = true;
        }
        else if (StartsWith(a, "-filemask=") || StartsWith(a, "-transfer=") ||
                 StartsWith(a, "-resume") || a == "-resume")
        {
            /* Accepted WinSCP switch compatibility */
        }
        else
        {
            paths.push_back(a);
        }
    }

    if (paths.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("put",
            "Syntax: put [switches] <localfile(s)> [<remotepath>]"));
        mErrors++;
        return false;
    }

    std::string destinationArg = paths.size() >= 2 ? paths.back() : ".";
    std::vector<std::string> sourcePatterns = paths.size() >= 2 ?
        std::vector<std::string>(paths.begin(), paths.end() - 1) :
        std::vector<std::string>(paths.begin(), paths.end());

    std::string destination = ResolveRemotePath(mRemoteDirectory, destinationArg);
    LscpAttrs destinationAttrs = {};
    const int destinationStat = lscp_sftp_stat(destination.c_str(), &destinationAttrs);
    if (destinationStat == LSCP_SFTP_STAT_ERROR)
    {
        Console::Instance().Error(Texts::RemoteError("inspect destination", destination,
            lscp_sftp_error()));
        mErrors++;
        return false;
    }
    bool destinationExists = destinationStat == LSCP_SFTP_STAT_EXISTS;
    bool destinationIsDir = destinationExists &&
        (destinationAttrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
        (destinationAttrs.permissions & 0040000) != 0;

    bool any = false;
    for (size_t i = 0; i < sourcePatterns.size(); i++)
    {
        std::vector<std::string> sources;
        std::string error;
        std::string current = GetLocalDirectory(error);
        if (current.empty())
        {
            Console::Instance().Error(error);
            mErrors++;
            continue;
        }
        if (!ExpandLocalWildcard(ResolveLocalPath(current, sourcePatterns[i]), sources, error))
        {
            Console::Instance().Error(Texts::LocalError("expansion of", sourcePatterns[i], error));
            mErrors++;
            continue;
        }
        if (sources.empty())
        {
            Console::Instance().Error(Texts::LocalError("expansion of", sourcePatterns[i],
                "No such file."));
            mErrors++;
            continue;
        }
        if (sources.size() > 1 || sourcePatterns.size() > 1)
        {
            if (destinationExists && !destinationIsDir)
            {
                Console::Instance().Error(Texts::RemoteError("upload to", destination,
                    "A directory is required for multiple source files."));
                mErrors++;
                continue;
            }
            if (!destinationIsDir)
            {
                if (!CreateRemoteDirectoryRecursive(destination, error))
                {
                    Console::Instance().Error(Texts::RemoteError("create the directory",
                        destination, error));
                    mErrors++;
                    continue;
                }
                destinationExists = true;
                destinationIsDir = true;
            }
        }
        for (size_t n = 0; n < sources.size(); n++)
        {
            LocalFileInfo info = StatLocal(sources[n]);
            if (info.isDir)
            {
                if (!flags.recursive)
                {
                    Console::Instance().Error(Texts::LocalError("upload of", sources[n],
                        "Directories are not supported without recursive mode."));
                    mErrors++;
                    continue;
                }
                std::string targetRemoteDir = (destinationIsDir || sources.size() > 1 ||
                    sourcePatterns.size() > 1) ?
                    JoinRemotePath(destination, LocalFileName(sources[n])) : destination;

                unsigned long long dirBytes = 0;
                if (UploadDirectoryRecursive(sources[n], targetRemoteDir, flags, dirBytes))
                {
                    any = true;
                    if (flags.deleteSource)
                    {
                        rmdir(sources[n].c_str());
                    }
                }
                continue;
            }
            if (info.exists && !info.isRegular)
            {
                Console::Instance().Error(Texts::LocalError("upload of", sources[n],
                    "Only regular files can be uploaded."));
                mErrors++;
                continue;
            }
            std::string remotePath = (destinationIsDir || sources.size() > 1 ||
                sourcePatterns.size() > 1) ?
                JoinRemotePath(destination, LocalFileName(sources[n])) : destination;
            LscpAttrs remoteInfo = {};
            const int remoteStat = lscp_sftp_stat(remotePath.c_str(), &remoteInfo);
            if (remoteStat == LSCP_SFTP_STAT_ERROR)
            {
                Console::Instance().Error(Texts::RemoteError("inspect destination",
                    remotePath, lscp_sftp_error()));
                mErrors++;
                continue;
            }
            const bool remoteExists = remoteStat == LSCP_SFTP_STAT_EXISTS;
            if (remoteExists && (remoteInfo.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
                (remoteInfo.permissions & 0040000) != 0)
            {
                Console::Instance().Error(Texts::RemoteError("upload to", remotePath,
                    "The destination is a directory."));
                mErrors++;
                continue;
            }
            if (flags.newerOnly && remoteExists &&
                (remoteInfo.flags & LSCP_ATTR_ACMODTIME) != 0 &&
                remoteInfo.mtime >= info.mtime)
            {
                continue;
            }
            if (mConfirm && remoteExists)
            {
                ConfirmationResult confirmation = ConfirmDestructiveAction(
                    "Overwrite remote file '" + remotePath + "'?");
                if (confirmation != ConfirmationResult::Accepted)
                {
                    if (confirmation == ConfirmationResult::Unavailable)
                    {
                        mErrors++;
                    }
                    continue;
                }
            }
            unsigned long long size = 0;
            Console::Instance().TransferStart(LocalFileName(sources[n]), info.size);
            int result = lscp_sftp_put(sources[n].c_str(), remotePath.c_str(), -1,
                &size, ProgressCallback, NULL);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(sources[n],
                    lscp_sftp_error()));
                mErrors++;
                continue;
            }
            any = true;
            Console::Instance().Status(Texts::FileTransferred(remotePath, size));
            if (flags.deleteSource)
            {
                RemoveLocalFile(sources[n], error);
            }
        }
    }
    return any;
}

bool Script::SynchronizeDirectories(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }

    std::string mode = "local";
    TransferFlags flags;
    flags.newerOnly = true;
    flags.recursive = true;
    std::vector<std::string> dirs;

    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & a = args[i];
        if (a == "-delete")
        {
            flags.deleteSource = true;
        }
        else if (i == 0 && (ToLower(a) == "local" || ToLower(a) == "remote" || ToLower(a) == "both"))
        {
            mode = ToLower(a);
        }
        else
        {
            dirs.push_back(a);
        }
    }

    std::string error;
    std::string localDir = dirs.size() > 0 ? ExpandTilde(dirs[0]) : GetLocalDirectory(error);
    std::string remoteDir = dirs.size() > 1 ? ResolveRemotePath(mRemoteDirectory, dirs[1]) : mRemoteDirectory;

    if (localDir.empty())
    {
        Console::Instance().Error("Cannot determine local directory: " + error);
        mErrors++;
        return false;
    }
    if (remoteDir.empty())
    {
        remoteDir = "/";
    }

    Console::Instance().Status("Synchronizing (" + mode + ") '" + localDir + "' and '" + remoteDir + "'...");

    unsigned long long bytes = 0;
    bool ok = true;
    if (mode == "remote" || mode == "both")
    {
        ok = UploadDirectoryRecursive(localDir, remoteDir, flags, bytes) && ok;
    }
    if (mode == "local" || mode == "both")
    {
        ok = DownloadDirectoryRecursive(remoteDir, localDir, flags, bytes) && ok;
    }

    Console::Instance().Status("Synchronization completed.");
    return ok;
}

bool Script::RemoveRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("rm", "Syntax: rm <file(s)>"));
        mErrors++;
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < args.size(); i++)
    {
        std::vector<std::string> paths;
        std::string error;
        if (!ExpandRemote(args[i], paths, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", args[i], error));
            mErrors++;
            ok = false;
            continue;
        }
        if (paths.empty())
        {
            Console::Instance().Error(Texts::RemoteError("find", args[i],
                "No matching files."));
            mErrors++;
            ok = false;
            continue;
        }
        for (size_t n = 0; n < paths.size(); n++)
        {
            if (mConfirm)
            {
                ConfirmationResult confirmation = ConfirmDestructiveAction(
                    "Delete remote file '" + paths[n] + "'?");
                if (confirmation != ConfirmationResult::Accepted)
                {
                    if (confirmation == ConfirmationResult::Unavailable)
                    {
                        mErrors++;
                        ok = false;
                    }
                    continue;
                }
            }
            if (lscp_sftp_remove(paths[n].c_str()) != 0)
            {
                Console::Instance().Error(Texts::RemoteError("remove", paths[n],
                    lscp_sftp_error()));
                mErrors++;
                ok = false;
            }
        }
    }
    return ok;
}

bool Script::RemoveRemoteDirectory(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 1)
    {
        Console::Instance().Error(Texts::WrongArguments("rmdir",
            "Syntax: rmdir <directory>"));
        mErrors++;
        return false;
    }
    std::string path = ResolveRemotePath(mRemoteDirectory, args[0]);
    if (mConfirm)
    {
        ConfirmationResult confirmation = ConfirmDestructiveAction(
            "Delete remote directory '" + path + "'?");
        if (confirmation != ConfirmationResult::Accepted)
        {
            if (confirmation == ConfirmationResult::Unavailable)
            {
                mErrors++;
                return false;
            }
            return true;
        }
    }
    if (lscp_sftp_rmdir(path.c_str()) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("remove the directory", path,
            lscp_sftp_error()));
        mErrors++;
        return false;
    }
    return true;
}

bool Script::MakeRemoteDirectory(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 1)
    {
        Console::Instance().Error(Texts::WrongArguments("mkdir",
            "Syntax: mkdir <directory>"));
        mErrors++;
        return false;
    }
    std::string path = ResolveRemotePath(mRemoteDirectory, args[0]);
    if (lscp_sftp_mkdir(path.c_str(), -1) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("create the directory", path,
            lscp_sftp_error()));
        mErrors++;
        return false;
    }
    return true;
}

bool Script::MoveRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments("mv",
            "Syntax: mv <source> <target>"));
        mErrors++;
        return false;
    }
    std::string from = ResolveRemotePath(mRemoteDirectory, args[0]);
    std::string to = ResolveRemotePath(mRemoteDirectory, args[1]);
    if (mConfirm)
    {
        LscpAttrs existing = {};
        const int targetStat = lscp_sftp_stat(to.c_str(), &existing);
        if (targetStat == LSCP_SFTP_STAT_ERROR)
        {
            Console::Instance().Error(Texts::RemoteError("inspect target", to,
                lscp_sftp_error()));
            mErrors++;
            return false;
        }
        if (targetStat == LSCP_SFTP_STAT_EXISTS)
        {
            ConfirmationResult confirmation = ConfirmDestructiveAction(
                "Overwrite remote file '" + to + "'?");
            if (confirmation != ConfirmationResult::Accepted)
            {
                if (confirmation == ConfirmationResult::Unavailable)
                {
                    mErrors++;
                    return false;
                }
                return true;
            }
        }
    }
    if (lscp_sftp_rename(from.c_str(), to.c_str()) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("move", from, lscp_sftp_error()));
        mErrors++;
        return false;
    }
    return true;
}

bool Script::ChangeRemotePermissions(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() < 2)
    {
        Console::Instance().Error(Texts::WrongArguments("chmod",
            "Syntax: chmod <permissions> <file(s)>"));
        mErrors++;
        return false;
    }
    unsigned long permissions = 0;
    if (!ParseOctalPermissions(args[0], permissions))
    {
        Console::Instance().Error(Texts::WrongArguments("chmod",
            "Permissions must be an octal number from 0 through 7777, for example 644."));
        mErrors++;
        return false;
    }
    bool ok = true;
    for (size_t i = 1; i < args.size(); i++)
    {
        std::vector<std::string> paths;
        std::string error;
        if (!ExpandRemote(args[i], paths, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", args[i], error));
            mErrors++;
            ok = false;
            continue;
        }
        if (paths.empty())
        {
            Console::Instance().Error(Texts::RemoteError("find", args[i],
                "No matching files."));
            mErrors++;
            ok = false;
            continue;
        }
        for (size_t n = 0; n < paths.size(); n++)
        {
            if (lscp_sftp_set_permissions(paths[n].c_str(), (int)permissions) != 0)
            {
                Console::Instance().Error(Texts::RemoteError("change the permissions of",
                    paths[n], lscp_sftp_error()));
                mErrors++;
                ok = false;
            }
        }
    }
    return ok;
}

bool Script::StatRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 1)
    {
        Console::Instance().Error(Texts::WrongArguments("stat", "Syntax: stat <file>"));
        mErrors++;
        return false;
    }
    std::string path = ResolveRemotePath(mRemoteDirectory, args[0]);
    LscpAttrs attrs;
    if (lscp_sftp_stat(path.c_str(), &attrs) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("read the attributes of", path,
            lscp_sftp_error()));
        mErrors++;
        return false;
    }
    Console::Instance().WriteLine("Path:        " + path);
    Console::Instance().WriteLine("Size:        " + FormatSize(attrs.size));
    Console::Instance().WriteLine("Permissions: " + FormatPermissions(attrs.permissions));
    Console::Instance().WriteLine("Modified:    " + FormatFileTime(attrs.mtime));
    Console::Instance().WriteLine("Owner:       " + std::string(
        attrs.flags & LSCP_ATTR_UIDGID ?
        std::to_string(attrs.uid) + ":" + std::to_string(attrs.gid) : std::string("unknown")));
    return true;
}

bool Script::SetOption(const std::vector<std::string> & args)
{
    if (args.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("option",
            "Syntax: option <name> <value> (supported: confirm, batch, reconnect, transfer)"));
        mErrors++;
        return false;
    }
    std::string name = ToLower(args[0]);
    if (args.size() == 1)
    {
        if (name == "confirm")
        {
            Console::Instance().Status(std::string("confirm = ") + (mConfirm ? "on" : "off"));
            return true;
        }
        if (name == "batch")
        {
            Console::Instance().Status(std::string("batch = ") + (Console::Instance().IsBatchMode() ? "on" : "off"));
            return true;
        }
    }
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments("option",
            "Syntax: option <name> <value> (supported: confirm, batch, reconnect, transfer)"));
        mErrors++;
        return false;
    }
    std::string value = ToLower(args[1]);
    if (name == "confirm")
    {
        if (value == "on")
        {
            mConfirm = true;
        }
        else if (value == "off")
        {
            mConfirm = false;
        }
        else
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'confirm' must be 'on' or 'off'."));
            mErrors++;
            return false;
        }
        return true;
    }
    if (name == "batch")
    {
        if (value == "on" || value == "abort")
        {
            Console::Instance().SetBatchMode(true);
        }
        else if (value == "off")
        {
            Console::Instance().SetBatchMode(false);
        }
        else
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'batch' must be 'on', 'off' or 'abort'."));
            mErrors++;
            return false;
        }
        return true;
    }
    if (name == "reconnect")
    {
        Console::Instance().Status("Option 'reconnect' set to " + args[1]);
        return true;
    }
    if (name == "transfer")
    {
        Console::Instance().Status("Option 'transfer' set to " + args[1]);
        return true;
    }
    Console::Instance().Error(Texts::WrongArguments("option",
        "The option '" + args[0] + "' is not supported by this build."));
    mErrors++;
    return false;
}

void Script::PrintHelp()
{
    Console::Instance().WriteLine(Texts::Usage());
}

bool Script::ExecuteLine(const std::string & line)
{
    std::string trimmed = line;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || trimmed[start] == '#')
    {
        return true;
    }
    trimmed = trimmed.substr(start);

    std::vector<std::string> words = SplitCommandLine(trimmed);
    if (words.empty())
    {
        return true;
    }
    std::string command = ToLower(words[0]);
    std::vector<std::string> args(words.begin() + 1, words.end());

    if (command == "exit" || command == "bye" || command == "quit" || command == "end")
    {
        mExit = true;
        return false;
    }
    if (command == "help" || command == "?")
    {
        PrintHelp();
        return true;
    }
    if (command == "echo")
    {
        std::string message;
        for (size_t i = 0; i < args.size(); i++)
        {
            if (i > 0)
            {
                message += " ";
            }
            message += args[i];
        }
        Console::Instance().WriteLine(message);
        return true;
    }
    if (command == "open")
    {
        OpenSite(args);
        return true;
    }
    if (command == "close")
    {
        CloseSite();
        return true;
    }
    if (command == "cd")
    {
        ChangeRemoteDirectory(args);
        return true;
    }
    if (command == "pwd")
    {
        PrintRemoteDirectory();
        return true;
    }
    if (command == "ls" || command == "dir")
    {
        ListRemote(args);
        return true;
    }
    if (command == "lcd")
    {
        ChangeLocalDirectory(args);
        return true;
    }
    if (command == "lpwd")
    {
        PrintLocalDirectory();
        return true;
    }
    if (command == "get")
    {
        GetFiles(args);
        return true;
    }
    if (command == "put")
    {
        PutFiles(args);
        return true;
    }
    if (command == "synchronize" || command == "sync")
    {
        SynchronizeDirectories(args);
        return true;
    }
    if (command == "rm" || command == "del")
    {
        RemoveRemote(args);
        return true;
    }
    if (command == "rmdir")
    {
        RemoveRemoteDirectory(args);
        return true;
    }
    if (command == "mkdir" || command == "md")
    {
        MakeRemoteDirectory(args);
        return true;
    }
    if (command == "mv" || command == "rename")
    {
        MoveRemote(args);
        return true;
    }
    if (command == "chmod")
    {
        ChangeRemotePermissions(args);
        return true;
    }
    if (command == "stat")
    {
        StatRemote(args);
        return true;
    }
    if (command == "option")
    {
        SetOption(args);
        return true;
    }
    if (command == "call" || command == "show")
    {
        Console::Instance().Error(Texts::WrongArguments(command,
            "The connection runs the SFTP subsystem, so remote shell commands "
            "are not available in this build."));
        mErrors++;
        return true;
    }

    Console::Instance().Error(Texts::UnknownCommand(command));
    mErrors++;
    return true;
}

int Script::RunFile(const std::string & path)
{
    FILE * file = stdin;
    bool closeFile = false;
    if (path != "-")
    {
        file = fopen(path.c_str(), "r");
        if (file == NULL)
        {
            Console::Instance().Error("Cannot open the script file '" + path + "': " +
                LastSystemError());
            return 1;
        }
        closeFile = true;
    }

    while (!mExit)
    {
        std::string line;
        bool sawInput = false;
        int c = EOF;
        while ((c = fgetc(file)) != EOF)
        {
            sawInput = true;
            if (c == '\n')
            {
                break;
            }
            if (c != '\r')
            {
                line += static_cast<char>(c);
            }
        }
        if (!sawInput && c == EOF)
        {
            break;
        }
        ExecuteLine(line);
        if (c == EOF)
        {
            break;
        }
    }
    if (ferror(file))
    {
        Console::Instance().Error("Error while reading the script file '" + path + "'.");
        mErrors++;
    }
    if (closeFile && fclose(file) != 0)
    {
        Console::Instance().Error("Error while closing the script file '" + path + "'.");
        mErrors++;
    }
    return mErrors != 0 ? 1 : 0;
}

int Script::RunCommand(const std::string & line)
{
    /* '-command=' may carry several commands separated by ';'. */
    std::vector<std::string> commands = SplitCommands(line);
    for (size_t i = 0; i < commands.size(); i++)
    {
        if (!commands[i].empty())
        {
            ExecuteLine(commands[i]);
        }
        if (mExit)
        {
            break;
        }
    }
    return mErrors != 0 ? 1 : 0;
}

int Script::RunInteractive(const std::string & initialUrl)
{
    if (!initialUrl.empty())
    {
        std::vector<std::string> args;
        args.push_back(initialUrl);
        OpenSite(args);
    }
    Console & console = Console::Instance();
    std::string line;
    while (!mExit)
    {
        if (!console.ReadLine("sftp> ", line))
        {
            break;
        }
        ExecuteLine(line);
    }
    return mErrors != 0 ? 1 : 0;
}

} // namespace linuxscp
