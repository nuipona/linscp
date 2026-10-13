#include "Script.h"
#include "ProtocolClient.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>

#include "Console.h"
#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"
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

bool StringListContains(const std::vector<std::string> & list,
    const std::string & value)
{
    for (size_t i = 0; i < list.size(); i++)
    {
        if (list[i] == value)
        {
            return true;
        }
    }
    return false;
}

bool NameListContains(const LscpNames * names, const std::string & value)
{
    if (names == NULL)
    {
        return false;
    }
    for (int i = 0; i < names->count; i++)
    {
        if (names->names[i].filename != NULL && value == names->names[i].filename)
        {
            return true;
        }
    }
    return false;
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
    mResume(false),
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

bool Script::RemoteListDir(const std::string & path, LscpNames *& names, std::string & error)
{
    error.clear();
    names = NULL;
    if (UseNativeSftp(mSession.Options().protocol))
    {
        names = lscp_sftp_list_dir(path.c_str());
        if (names == NULL)
        {
            error = lscp_sftp_error();
            return false;
        }
        return true;
    }
    names = new LscpNames();
    bool ok = ProtocolClient::List(mSession.Options(), path, names, error);
    if (!ok)
    {
        delete names;
        names = NULL;
        return false;
    }
    return true;
}

void Script::RemoteFreeNames(LscpNames * names)
{
    if (names == NULL)
    {
        return;
    }
    if (UseNativeSftp(mSession.Options().protocol))
    {
        lscp_sftp_free_names(names);
    }
    else
    {
        ProtocolClient::FreeNames(names);
        delete names;
    }
}

int Script::RemoteStat(const std::string & path, LscpAttrs * attrs, std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int st = lscp_sftp_stat(path.c_str(), attrs);
        if (st != LSCP_SFTP_STAT_EXISTS)
        {
            error = lscp_sftp_error();
        }
        return st;
    }
    return ProtocolClient::Stat(mSession.Options(), path, attrs, error);
}

int Script::RemoteGet(const std::string & remote, const std::string & local,
    unsigned long long resumeOffset, bool preserveTime,
    unsigned long long * size, LscpProgressFn progress, void * ctx,
    std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_get_resume(remote.c_str(), local.c_str(), resumeOffset, size,
            progress, ctx);
        if (res != 0)
        {
            error = lscp_sftp_error();
            return res;
        }
        if (preserveTime)
        {
            LscpAttrs attrs = {};
            if (lscp_sftp_stat(remote.c_str(), &attrs) == LSCP_SFTP_STAT_EXISTS &&
                (attrs.flags & LSCP_ATTR_ACMODTIME) != 0 && attrs.mtime != 0)
            {
                /* Some servers report an atime of zero; keep the
                 * modification time in that case. */
                SetFileTimes(local, attrs.atime != 0 ? attrs.atime : attrs.mtime,
                    attrs.mtime);
            }
        }
        return res;
    }
    return ProtocolClient::Get(mSession.Options(), remote, local, resumeOffset,
        preserveTime, size, progress, ctx, error);
}

int Script::RemotePut(const std::string & local, const std::string & remote,
    int permissions, unsigned long long resumeOffset, bool preserveTime,
    unsigned long long * size, LscpProgressFn progress, void * ctx,
    std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_put_resume(local.c_str(), remote.c_str(), permissions,
            resumeOffset, size, progress, ctx);
        if (res != 0)
        {
            error = lscp_sftp_error();
            return res;
        }
        if (preserveTime)
        {
            /* The SFTP protocol has no way to carry the timestamps in the
             * transfer itself, so set them once the file is complete. */
            const LocalFileInfo info = StatLocal(local);
            if (info.exists && info.mtime != 0 &&
                lscp_sftp_set_times(remote.c_str(),
                    info.atime != 0 ? info.atime : info.mtime, info.mtime) != 0)
            {
                Console::Instance().Status("Could not set the timestamps of '" +
                    remote + "': " + lscp_sftp_error());
            }
        }
        return res;
    }
    return ProtocolClient::Put(mSession.Options(), local, remote, permissions,
        resumeOffset, preserveTime, size, progress, ctx, error);
}

int Script::RemoteRemove(const std::string & path, std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_remove(path.c_str());
        if (res != 0)
        {
            error = lscp_sftp_error();
        }
        return res;
    }
    return ProtocolClient::Remove(mSession.Options(), path, error);
}

int Script::RemoteRemoveDir(const std::string & path, std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_rmdir(path.c_str());
        if (res != 0)
        {
            error = lscp_sftp_error();
        }
        return res;
    }
    return ProtocolClient::RemoveDir(mSession.Options(), path, error);
}

int Script::RemoteMakeDir(const std::string & path, int permissions, std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_mkdir(path.c_str(), permissions);
        if (res != 0)
        {
            error = lscp_sftp_error();
        }
        return res;
    }
    return ProtocolClient::MakeDir(mSession.Options(), path, error);
}

int Script::RemoteRename(const std::string & from, const std::string & to, std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_rename(from.c_str(), to.c_str());
        if (res != 0)
        {
            error = lscp_sftp_error();
        }
        return res;
    }
    return ProtocolClient::Move(mSession.Options(), from, to, error);
}

int Script::RemoteSetPermissions(const std::string & path, int permissions,
    std::string & error)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = lscp_sftp_set_permissions(path.c_str(), permissions);
        if (res != 0)
        {
            error = lscp_sftp_error();
        }
        return res;
    }
    return ProtocolClient::SetPermissions(mSession.Options(), path, permissions,
        error);
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
        options.userName = CurrentUserName();
    }

    Console::Instance().Status("Connecting to " + options.hostName + "...");
    if (!mSession.Open(options, error))
    {
        Console::Instance().Error(Texts::CannotOpenSession(options.hostName, error));
        mErrors++;
        return false;
    }

    if (UseNativeSftp(mSession.Options().protocol))
    {
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
    }
    else
    {
        mRemoteDirectory = options.initialDirectory.empty() ?
            std::string("/") : options.initialDirectory;
    }

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
    if (UseNativeSftp(mSession.Options().protocol))
    {
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
    }
    else
    {
        mRemoteDirectory = path;
    }
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
    LscpNames * names = NULL;
    std::string readError;
    if (!RemoteListDir(path, names, readError))
    {
        Console::Instance().Error(Texts::CannotReadDirectory(path, readError));
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
    RemoteFreeNames(names);
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
    LscpNames * names = NULL;
    if (!RemoteListDir(dir, names, error))
    {
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
    RemoteFreeNames(names);
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
    int st = RemoteStat(path, &attrs, error);
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
    if (RemoteMakeDir(path, -1, error) != 0)
    {
        return false;
    }
    return true;
}

bool Script::ConfirmObsolete(const std::string & side, const std::string & path,
    bool isDirectory)
{
    if (!mConfirm)
    {
        return true;
    }
    const ConfirmationResult confirmation = ConfirmDestructiveAction(
        "Delete obsolete " + side + (isDirectory ? " directory '" : " file '") +
        path + "'?");
    if (confirmation == ConfirmationResult::Accepted)
    {
        return true;
    }
    if (confirmation == ConfirmationResult::Unavailable)
    {
        mErrors++;
    }
    return false;
}

bool Script::RemoveLocalItemRecursive(const std::string & path)
{
    const LocalFileInfo info = StatLocal(path);
    std::string error;
    if (info.isDir)
    {
        std::vector<std::string> entries;
        if (!ListLocalDirectory(path, entries, error))
        {
            Console::Instance().Error(Texts::LocalError("read directory", path, error));
            mErrors++;
            return false;
        }
        bool success = true;
        for (size_t i = 0; i < entries.size(); i++)
        {
            if (!RemoveLocalItemRecursive(JoinLocalPath(path, entries[i])))
            {
                success = false;
            }
        }
        if (!RemoveLocalDirectory(path, error))
        {
            Console::Instance().Error(Texts::LocalError("remove directory", path, error));
            mErrors++;
            return false;
        }
        Console::Instance().Status("Removed local directory '" + path + "'.");
        return success;
    }
    if (!RemoveLocalFile(path, error))
    {
        Console::Instance().Error(Texts::LocalError("remove file", path, error));
        mErrors++;
        return false;
    }
    Console::Instance().Status("Removed local file '" + path + "'.");
    return true;
}

bool Script::RemoveRemoteItemRecursive(const std::string & path)
{
    LscpAttrs attrs = {};
    std::string error;
    const int st = RemoteStat(path, &attrs, error);
    const bool isDir = st == LSCP_SFTP_STAT_EXISTS &&
        (attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
        (attrs.permissions & 0040000) != 0;
    if (isDir)
    {
        LscpNames * names = NULL;
        std::string listError;
        if (!RemoteListDir(path, names, listError))
        {
            Console::Instance().Error(Texts::RemoteError("read directory", path,
                listError));
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
            const std::string name(rawName);
            if (name == "." || name == "..")
            {
                continue;
            }
            if (!RemoveRemoteItemRecursive(JoinRemotePath(path, name)))
            {
                success = false;
            }
        }
        RemoteFreeNames(names);
        if (RemoteRemoveDir(path, error) != 0)
        {
            Console::Instance().Error(Texts::RemoteError("remove directory", path,
                error));
            mErrors++;
            return false;
        }
        Console::Instance().Status("Removed remote directory '" + path + "'.");
        return success;
    }
    if (RemoteRemove(path, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("remove file", path, error));
        mErrors++;
        return false;
    }
    Console::Instance().Status("Removed remote file '" + path + "'.");
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

    LscpNames * names = NULL;
    std::string dirErr;
    if (!RemoteListDir(remoteDir, names, dirErr))
    {
        Console::Instance().Error(Texts::RemoteError("read directory", remoteDir, dirErr));
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
            if (RemoteStat(childRemote, &a, dirErr) == LSCP_SFTP_STAT_EXISTS)
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
                RemoteRemoveDir(childRemote, dirErr);
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
            int result = RemoteGet(childRemote, childLocal, 0, flags.preserveTime,
                &size, ProgressCallback, NULL, dirErr);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(childRemote, dirErr));
                mErrors++;
                success = false;
                continue;
            }
            bytesTransferred += size;
            Console::Instance().Status(Texts::FileTransferred(childLocal, size));
            if (flags.preserveTime && (names->names[i].attrs.flags & LSCP_ATTR_ACMODTIME) != 0)
            {
                SetFileTimes(childLocal, names->names[i].attrs.atime,
                    names->names[i].attrs.mtime);
            }
            if (flags.deleteSource)
            {
                RemoteRemove(childRemote, dirErr);
            }
        }
    }

    /*
     * 'synchronize -delete': everything in the local target directory
     * that the remote source does not have is obsolete. The source is
     * never touched.
     */
    if (flags.deleteObsolete)
    {
        std::vector<std::string> localEntries;
        std::string listError;
        if (!ListLocalDirectory(localDir, localEntries, listError))
        {
            Console::Instance().Error(Texts::LocalError("read directory", localDir,
                listError));
            mErrors++;
            success = false;
        }
        for (size_t i = 0; i < localEntries.size(); i++)
        {
            if (NameListContains(names, localEntries[i]))
            {
                continue;
            }
            const std::string obsolete = JoinLocalPath(localDir, localEntries[i]);
            const bool obsoleteIsDir = StatLocal(obsolete).isDir;
            if (!ConfirmObsolete("local", obsolete, obsoleteIsDir))
            {
                success = false;
                continue;
            }
            if (!RemoveLocalItemRecursive(obsolete))
            {
                success = false;
            }
        }
    }

    RemoteFreeNames(names);
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
                std::string removeError;
                RemoveLocalDirectory(childLocal, removeError);
            }
        }
        else if (info.isRegular)
        {
            LscpAttrs remoteInfo = {};
            const int remoteStat = RemoteStat(childRemote, &remoteInfo, error);
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
            int result = RemotePut(childLocal, childRemote, -1, 0,
                flags.preserveTime, &size, ProgressCallback, NULL, error);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(childLocal, error));
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

    /*
     * 'synchronize -delete': remove the remote target entries that the
     * local source does not have. The source files stay where they are.
     */
    if (flags.deleteObsolete)
    {
        LscpNames * remoteNames = NULL;
        std::string listError;
        if (!RemoteListDir(remoteDir, remoteNames, listError))
        {
            Console::Instance().Error(Texts::RemoteError("read directory", remoteDir,
                listError));
            mErrors++;
            return false;
        }
        for (int i = 0; i < remoteNames->count; i++)
        {
            const char * rawName = remoteNames->names[i].filename;
            if (rawName == NULL)
            {
                continue;
            }
            const std::string name(rawName);
            if (name == "." || name == ".." ||
                StringListContains(entries, name))
            {
                continue;
            }
            const std::string obsolete = JoinRemotePath(remoteDir, name);
            const bool obsoleteIsDir =
                (remoteNames->names[i].attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
                (remoteNames->names[i].attrs.permissions & 0040000) != 0;
            if (!ConfirmObsolete("remote", obsolete, obsoleteIsDir))
            {
                success = false;
                continue;
            }
            if (!RemoveRemoteItemRecursive(obsolete))
            {
                success = false;
            }
        }
        RemoteFreeNames(remoteNames);
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
        else if (StartsWith(a, "-resume") || a == "-resume")
        {
            flags.resume = true;
        }
        else if (StartsWith(a, "-filemask=") || StartsWith(a, "-transfer="))
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
            int statRes = RemoteStat(sources[n], &sourceAttrs, error);
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
                        RemoteRemoveDir(sources[n], error);
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
            if (mConfirm && localInfo.exists && !(flags.resume || mResume))
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
            unsigned long long resumeOffset = 0;
            if ((flags.resume || mResume) && localInfo.exists && localInfo.isRegular && localInfo.size < totalSize)
            {
                resumeOffset = localInfo.size;
                Console::Instance().Status("Resuming download of '" + RemoteFileName(sources[n]) +
                    "' from byte " + std::to_string(resumeOffset));
            }
            unsigned long long size = 0;
            Console::Instance().TransferStart(RemoteFileName(sources[n]), totalSize);
            int result = RemoteGet(sources[n], localPath, resumeOffset,
                flags.preserveTime, &size, ProgressCallback, NULL, error);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(sources[n], error));
                mErrors++;
                continue;
            }
            any = true;
            Console::Instance().Status(Texts::FileTransferred(localPath, size));
            if (flags.preserveTime && (sourceAttrs.flags & LSCP_ATTR_ACMODTIME) != 0)
            {
                SetFileTimes(localPath, sourceAttrs.atime, sourceAttrs.mtime);
            }
            if (flags.deleteSource)
            {
                RemoteRemove(sources[n], error);
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
        else if (StartsWith(a, "-resume") || a == "-resume")
        {
            flags.resume = true;
        }
        else if (StartsWith(a, "-filemask=") || StartsWith(a, "-transfer="))
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
    std::string destErr;
    const int destinationStat = RemoteStat(destination, &destinationAttrs, destErr);
    if (destinationStat == LSCP_SFTP_STAT_ERROR)
    {
        Console::Instance().Error(Texts::RemoteError("inspect destination", destination,
            destErr));
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
                        std::string removeError;
                        RemoveLocalDirectory(sources[n], removeError);
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
            const int remoteStat = RemoteStat(remotePath, &remoteInfo, error);
            if (remoteStat == LSCP_SFTP_STAT_ERROR)
            {
                Console::Instance().Error(Texts::RemoteError("inspect destination",
                    remotePath, error));
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
            if (mConfirm && remoteExists && !(flags.resume || mResume))
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
            unsigned long long resumeOffset = 0;
            if ((flags.resume || mResume) && remoteExists && remoteInfo.size < info.size)
            {
                resumeOffset = remoteInfo.size;
                Console::Instance().Status("Resuming upload of '" + LocalFileName(sources[n]) +
                    "' from byte " + std::to_string(resumeOffset));
            }
            unsigned long long size = 0;
            Console::Instance().TransferStart(LocalFileName(sources[n]), info.size);
            int result = RemotePut(sources[n], remotePath, -1,
                resumeOffset, flags.preserveTime, &size, ProgressCallback, NULL,
                error);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(sources[n], error));
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
            /* WinSCP semantics: delete the target entries the other side
             * does not have. The source is never removed by a
             * synchronization. */
            flags.deleteObsolete = true;
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
            if (RemoteRemove(paths[n], error) != 0)
            {
                Console::Instance().Error(Texts::RemoteError("remove", paths[n],
                    error));
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
    std::string error;
    if (RemoteRemoveDir(path, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("remove the directory", path,
            error));
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
    std::string error;
    if (RemoteMakeDir(path, -1, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("create the directory", path,
            error));
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
    std::string error;
    if (mConfirm)
    {
        LscpAttrs existing = {};
        const int targetStat = RemoteStat(to, &existing, error);
        if (targetStat == LSCP_SFTP_STAT_ERROR)
        {
            Console::Instance().Error(Texts::RemoteError("inspect target", to,
                error));
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
    if (RemoteRename(from, to, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("move", from, error));
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
            std::string permError;
            if (RemoteSetPermissions(paths[n], (int)permissions, permError) != 0)
            {
                Console::Instance().Error(Texts::RemoteError("change the permissions of",
                    paths[n], permError));
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
    std::string error;
    if (RemoteStat(path, &attrs, error) != LSCP_SFTP_STAT_EXISTS)
    {
        Console::Instance().Error(Texts::RemoteError("read the attributes of", path,
            error));
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

/*
 * WinSCP's "Open Terminal" (Ctrl+P): an interactive shell on the server
 * that the session is connected to. This client's SSH core offers the
 * SFTP subsystem only, so the shell comes from the system ssh client in
 * a terminal window, which is also what makes the command useful in a
 * script file. The terminal program is picked by the platform layer
 * (LINUXSCP_TERMINAL overrides it on POSIX).
 */
void Script::OpenTerminal(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return;
    }
    if (!args.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("terminal",
            "The command takes no arguments."));
        mErrors++;
        return;
    }

    const SessionOptions & options = mSession.Options();
    if (options.protocol != "sftp")
    {
        Console::Instance().Error("An interactive terminal is only available "
            "for SSH (sftp://) sessions, not for '" + options.protocol + "'.");
        mErrors++;
        return;
    }

    std::string userName = mSession.UserName();
    if (userName.empty())
    {
        userName = options.userName;
    }

    /* A PuTTY key file cannot be read by the system ssh client, so the
     * terminal can only use the agent (or ask for a password). */
    const std::string key = ExpandTilde(options.privateKeyFile);
    if (key.size() >= 4)
    {
        std::string extension = key.substr(key.size() - 4);
        for (size_t i = 0; i < extension.size(); i++)
        {
            extension[i] = static_cast<char>(tolower(static_cast<unsigned char>(extension[i])));
        }
        if (extension == ".ppk")
        {
            Console::Instance().LogEvent("The PuTTY key file '" + key +
                "' is not passed to the system ssh client; the terminal will "
                "use the agent or ask for a password.");
        }
    }

    std::string error;
    if (!LaunchTerminalForServer(userName, options.hostName, options.portNumber,
            key, error))
    {
        Console::Instance().Error(error);
        mErrors++;
        return;
    }

    Console::Instance().WriteLine("Opened a terminal on " +
        (userName.empty() ? options.hostName : userName + "@" + options.hostName) + ".");
}

bool Script::SetOption(const std::vector<std::string> & args)
{
    if (args.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("option",
            "Syntax: option <name> <value> (supported: confirm, batch, resume, reconnect, transfer)"));
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
        if (name == "resume")
        {
            Console::Instance().Status(std::string("resume = ") + (mResume ? "on" : "off"));
            return true;
        }
    }
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments("option",
            "Syntax: option <name> <value> (supported: confirm, batch, resume, reconnect, transfer)"));
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
    if (name == "resume")
    {
        if (value == "on")
        {
            mResume = true;
        }
        else if (value == "off")
        {
            mResume = false;
        }
        else
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'resume' must be 'on' or 'off'."));
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
    if (command == "terminal")
    {
        OpenTerminal(args);
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
        file = fopen(path.c_str(), "rb");
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
