#include "Script.h"
#include "ProtocolClient.h"
#include "ScriptParams.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <chrono>

#if !defined(_WIN32)
/* The public half of a key file, which only the bundled PuTTY core can
 * read on POSIX. */
#include "PuttyCompat.h"
#endif

#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "Console.h"
#include "HashUtil.h"
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

/*
 * The command names the interpreter knows, in the order 'help' lists them.
 * Upstream keeps the same list in its command table and lets an unambiguous
 * prefix stand for a whole name; tools/check_upstream_parity.py fails when
 * the dispatcher grows a command that is not listed here.
 */
const char * const kCommandNames[] = {
    "!", "ascii", "binary", "bye", "call", "cd", "checksum", "chmod",
    "close", "copyid", "cp", "del", "delsession", "dir", "echo", "edit",
    "end", "exit", "get", "help", "keepuptodate", "lcd", "lls", "ln",
    "lpwd", "ls", "man", "md", "mkdir", "mget", "mput", "mv", "open",
    "option", "pwd", "put", "quit", "recv", "rename", "rm", "rmdir",
    "rmsession", "send", "session", "sessions", "stat", "savesession",
    "show", "symlink", "sync", "synchronize", "terminal", "?"
};

std::vector<std::string> CommandNames()
{
    return std::vector<std::string>(kCommandNames,
        kCommandNames + sizeof(kCommandNames) / sizeof(kCommandNames[0]));
}

/*
 * A command line as it may be written to a log file. A password can reach a
 * log in two ways - inside a site URL and as the value of a '-password='
 * switch - and both are masked here, because an action log is kept, shipped
 * and archived more readily than a terminal. Upstream masks the sensitive
 * parameters of its log the same way.
 */
std::string CensoredForLog(const std::string & line)
{
    std::string result = line;

    /* 'scheme://user:password@host' -> 'scheme://user:***@host' */
    size_t from = 0;
    while (true)
    {
        const size_t scheme = result.find("://", from);
        if (scheme == std::string::npos)
        {
            break;
        }
        const size_t tokenEnd = result.find_first_of(" \t\"'", scheme + 3);
        const size_t at = result.find('@', scheme + 3);
        from = tokenEnd == std::string::npos ? result.size() : tokenEnd;
        if (at == std::string::npos || (tokenEnd != std::string::npos && at > tokenEnd))
        {
            continue;
        }
        const size_t colon = result.find(':', scheme + 3);
        if (colon == std::string::npos || colon > at)
        {
            continue;
        }
        result.replace(colon + 1, at - colon - 1, "***");
        from = colon + 4;
    }

    /* '-password=value' -> '-password=***' (also quoted values). */
    const std::string lowered = ToLower(result);
    size_t switchAt = 0;
    while ((switchAt = lowered.find("-password=", switchAt)) != std::string::npos)
    {
        const size_t value = switchAt + strlen("-password=");
        if (value < result.size() && result[value] == '"')
        {
            const size_t quote = result.find('"', value + 1);
            if (quote != std::string::npos)
            {
                result.replace(value + 1, quote - value - 1, "***");
                switchAt = quote + 1;
                continue;
            }
        }
        size_t end = result.find(' ', value);
        if (end == std::string::npos)
        {
            end = result.size();
        }
        result.replace(value, end - value, "***");
        switchAt = value + 3;
    }
    return result;
}

bool IsKnownCommand(const std::string & name)
{
    for (size_t i = 0; i < sizeof(kCommandNames) / sizeof(kCommandNames[0]); i++)
    {
        if (name == kCommandNames[i])
        {
            return true;
        }
    }
    return false;
}

/*
 * The '-speed=' limit. It is applied here rather than in a backend: the
 * progress callback runs after every chunk of every transfer, so pausing in
 * it paces the native SFTP engine, the curl path and the ASCII conversion
 * alike, without either of them knowing about bandwidth.
 */
unsigned long long gSpeedLimitBytes = 0;
std::chrono::steady_clock::time_point gSpeedStarted;
unsigned long long gSpeedSeen = 0;

void SetSpeedLimit(unsigned long long bytesPerSecond)
{
    gSpeedLimitBytes = bytesPerSecond;
    gSpeedSeen = 0;
    gSpeedStarted = std::chrono::steady_clock::now();
}

void PaceTransfer(unsigned long long transferred)
{
    if (gSpeedLimitBytes == 0)
    {
        return;
    }
    if (transferred < gSpeedSeen)
    {
        /* A new file started: the schedule restarts with it. */
        gSpeedStarted = std::chrono::steady_clock::now();
    }
    gSpeedSeen = transferred;

    const unsigned long long microseconds =
        transferred * 1000000ULL / gSpeedLimitBytes;
    const std::chrono::steady_clock::time_point due =
        gSpeedStarted + std::chrono::microseconds(microseconds);
    while (due > std::chrono::steady_clock::now())
    {
        /* Short sleeps, so that Ctrl-C still stops a paced transfer. */
        const long long wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            due - std::chrono::steady_clock::now()).count();
        SleepMilliseconds(wait > 100 ? 100u : static_cast<unsigned>(wait > 0 ? wait : 1));
        if (Interrupted())
        {
            return;
        }
    }
}

/*
 * Keeps the limit of one command in force while its files transfer and clears
 * it afterwards, on every return path. 'member' mirrors the value for the
 * curl backends, which ask curl itself to pace the connection.
 */
class SpeedLimitForThisCommand
{
public:
    SpeedLimitForThisCommand(unsigned kBytesPerSecond, unsigned * member) :
        mMember(member)
    {
        SetSpeedLimit(kBytesPerSecond * 1024ULL);
        if (mMember != NULL)
        {
            *mMember = kBytesPerSecond;
        }
    }
    ~SpeedLimitForThisCommand()
    {
        SetSpeedLimit(0);
        if (mMember != NULL)
        {
            *mMember = 0;
        }
    }

private:
    SpeedLimitForThisCommand(const SpeedLimitForThisCommand &);
    SpeedLimitForThisCommand & operator=(const SpeedLimitForThisCommand &);

    unsigned * mMember;
};

void ProgressCallback(unsigned long long transferred, unsigned long long total, void * ctx)
{
    (void)total;
    (void)ctx;
    PaceTransfer(transferred);
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
    mBatchMode(Console::Instance().IsBatchMode() ? BatchMode::Abort : BatchMode::Off),
    mBatchConfigured(false),
    mConfirm(true),
    mResume(false),
    mExit(false),
    mErrors(0),
    mLastCommandFailed(false),
    mEditMaxSize(EditOptions::kDefaultMaxSize),
    mTransferMode(TransferMode::Binary),
    mOptionMaskUsed(false),
    mFailOnNoMatch(false),
    mEchoCommands(false),
    mReconnectSeconds(0),
    mSyncDeleteDefault(false),
    mSpeedLimit(0),
    mServerLoop(false)
{
}

bool Script::AbortAfterFileError() const
{
    /* 'batch on'/'batch abort' stop everything, including the remaining
     * files of a multi-file command; 'continue'/'off' go on. */
    return mBatchMode == BatchMode::Abort || mBatchMode == BatchMode::On;
}

bool Script::RequireConnection()
{
    if (mSession.IsOpen() && mSession.IsAlive())
    {
        return true;
    }
    /* The session died in the middle of the script. With
     * 'option reconnecttime' the client reopens it once (within the
     * configured number of seconds) and restores the directory. */
    if (mSession.IsOpen() && mReconnectSeconds > 0)
    {
        const SessionOptions options = mSession.Options();
        const std::string lastDirectory = mRemoteDirectory;
        Console::Instance().Status("The connection was lost; reconnecting to " +
            mSession.HostDescription() + "...");
        mSession.Close();
        std::string error;
        if (!mSession.Open(options, error))
        {
            Console::Instance().Error(Texts::CannotOpenSession(
                options.hostName, error));
            mErrors++;
            return false;
        }
        mRemoteDirectory = options.initialDirectory.empty() ?
            "/" : options.initialDirectory;
        if (!lastDirectory.empty() && lastDirectory != "/")
        {
            LscpAttrs attrs = {};
            if (RemoteStat(lastDirectory, &attrs, error) ==
                LSCP_SFTP_STAT_EXISTS)
            {
                mRemoteDirectory = lastDirectory;
            }
        }
        return true;
    }
    if (mSession.IsOpen())
    {
        mSession.Close();
        mRemoteDirectory.clear();
    }
    Console::Instance().Error(Texts::NotConnected());
    mErrors++;
    return false;
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
    std::string & error, bool append)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = append ?
            lscp_sftp_get_append(remote.c_str(), local.c_str(), size, progress, ctx) :
            lscp_sftp_get_resume(remote.c_str(), local.c_str(), resumeOffset, size,
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
        preserveTime, size, progress, ctx, error, mSpeedLimit);
}

int Script::RemotePut(const std::string & local, const std::string & remote,
    int permissions, unsigned long long resumeOffset, bool preserveTime,
    unsigned long long * size, LscpProgressFn progress, void * ctx,
    std::string & error, bool append)
{
    error.clear();
    if (UseNativeSftp(mSession.Options().protocol))
    {
        int res = append ?
            lscp_sftp_put_append(local.c_str(), remote.c_str(), permissions, size,
                progress, ctx) :
            lscp_sftp_put_resume(local.c_str(), remote.c_str(), permissions,
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
        resumeOffset, preserveTime, size, progress, ctx, error, mSpeedLimit);
}

/* The timestamps of a local file onto a remote path, used when a transfer
 * preserves the time of a directory as well as of its files. */
int Script::RemoteSetTimes(const std::string & path, unsigned long atime,
    unsigned long mtime, std::string & error)
{
    error.clear();
    if (!UseNativeSftp(mSession.Options().protocol))
    {
        error = "Setting the timestamps of a directory needs the native "
            "SFTP transport.";
        return -1;
    }
    const int res = lscp_sftp_set_times(path.c_str(), atime, mtime);
    if (res != 0)
    {
        error = lscp_sftp_error();
    }
    return res;
}

/* Download/Upload with the plan's transfer mode: 'ascii'/'automatic'
 * convert the line endings around the binary transfer, exactly like
 * get/put do. */
int Script::RemoteGetMode(const std::string & remote, const std::string & local,
    TransferMode mode, unsigned long long * size, std::string & error)
{
    const int result = RemoteGet(remote, local, 0, true, size,
        ProgressCallback, NULL, error);
    if (result != 0)
    {
        return result;
    }
    if (WantsAsciiConversion(mode, RemoteFileName(remote)))
    {
        unsigned long long converted = 0;
        if (!ConvertToUnixLineEndings(local, converted, error))
        {
            return -1;
        }
        if (size != NULL)
        {
            *size = converted;
        }
    }
    return 0;
}

int Script::RemotePutMode(const std::string & local, const std::string & remote,
    TransferMode mode, unsigned long long * size, std::string & error)
{
    if (!WantsAsciiConversion(mode, LocalFileName(local)))
    {
        return RemotePut(local, remote, -1, 0, true, size,
            ProgressCallback, NULL, error);
    }
    std::string tempPath;
    if (!MakeCRLFUploadCopy(local, tempPath, error))
    {
        return -1;
    }
    const int result = RemotePut(tempPath, remote, -1, 0, true, size,
        ProgressCallback, NULL, error);
    remove(tempPath.c_str());
    return result;
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

/* The three remote operations the editor needs. They are thin wrappers
 * so that Editor.cpp never has to know about the session. */
int Script::StatForEdit(const std::string & path, LscpAttrs & attrs, std::string & error)
{
    return RemoteStat(path, &attrs, error);
}

int Script::DownloadForEdit(const std::string & remote, const std::string & local,
    bool preserveTime, std::string & error)
{
    return RemoteGet(remote, local, 0, preserveTime, NULL, ProgressCallback, NULL, error);
}

int Script::UploadForEdit(const std::string & local, const std::string & remote,
    bool preserveTime, std::string & error)
{
    return RemotePut(local, remote, -1, 0, preserveTime, NULL, ProgressCallback,
        NULL, error);
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

    /* A stored session name is not a URL: 'open web1' connects to the
     * session that was saved under that name. */
    if (url.find("://") == std::string::npos && mSessions.Find(url) != NULL)
    {
        return OpenStoredSite(mSessions.Find(url)->name);
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

        /* The directory the server starts us in is the home directory,
         * whatever 'cd' target the site configuration asked for. */
        char * home = lscp_sftp_realpath(".");
        if (home != NULL)
        {
            mHomeDirectory = home;
            free(home);
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
        if (options.initialDirectory.empty())
        {
            mHomeDirectory = mRemoteDirectory;
        }
    }

    /* The log records where the session went, never how it was
     * authenticated: the password is simply not part of the element. */
    Console::Instance().LogAction("session",
        "action=\"open\" protocol=\"" + Console::EscapeActionText(options.protocol) +
        "\" host=\"" + Console::EscapeActionText(options.hostName) +
        "\" port=\"" + std::to_string(options.portNumber) +
        "\" user=\"" + Console::EscapeActionText(options.userName) +
        "\" directory=\"" + Console::EscapeActionText(mRemoteDirectory) + "\"");

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
    mHomeDirectory.clear();
    Console::Instance().LogAction("session", "action=\"close\"");
}

bool Script::ChangeRemoteDirectory(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() > 1)
    {
        Console::Instance().Error(Texts::WrongArguments("cd",
            "Syntax: cd [<directory>]"));
        mErrors++;
        return false;
    }
    /* Without an argument, go to the home directory, like upstream. */
    std::string path = args.empty() ? mHomeDirectory :
        ResolveRemotePath(mRemoteDirectory, args[0]);
    if (path.empty())
    {
        path = "/";
    }
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
    Console::Instance().WriteLine(mRemoteDirectory);
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
                        "Syntax: ls [-al1] [<directory>[/<mask>]]"));
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
                "Syntax: ls [-al1] [<directory>[/<mask>]]"));
            mErrors++;
            return false;
        }
    }
    /* A trailing component with wildcards is a mask: list the parent
     * directory and show only the matching entries. */
    std::string mask;
    if (!target.empty())
    {
        const std::string leaf = RemoteFileName(target);
        if (HasWildcard(leaf))
        {
            mask = leaf;
            target = RemoteDirName(target);
            if (target.empty() || target == "/")
            {
                target.clear();
            }
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
        if (!mask.empty() && !MatchesMask(mask, name))
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
    unsigned long long & bytesTransferred, const std::string & relPrefix)
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

        const std::string childRel = relPrefix.empty() ? name :
            relPrefix + "/" + name;
        const unsigned long long childSize =
            (names->names[i].attrs.flags & LSCP_ATTR_SIZE) != 0 ?
            names->names[i].attrs.size : 0;
        const unsigned long childTime =
            (names->names[i].attrs.flags & LSCP_ATTR_ACMODTIME) != 0 ?
            names->names[i].attrs.mtime : 0;

        if (childIsDir)
        {
            /* A directory is pruned only by an exclusion. */
            if (!flags.mask.empty() &&
                !flags.mask.Accepts(name, true, 0, 0, childRel))
            {
                continue;
            }
            if (!DownloadDirectoryRecursive(childRemote, childLocal, flags,
                bytesTransferred, childRel))
            {
                success = false;
                if (AbortAfterFileError())
                {
                    RemoteFreeNames(names);
                    return false;
                }
            }
            if (flags.deleteSource)
            {
                RemoteRemoveDir(childRemote, dirErr);
            }
        }
        else
        {
            if (!flags.mask.empty() &&
                !flags.mask.Accepts(name, false, childSize, childTime, childRel))
            {
                continue;
            }
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
                &size, ProgressCallback, NULL, dirErr, flags.append);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(childRemote, dirErr));
                mErrors++;
                success = false;
                if (AbortAfterFileError())
                {
                    RemoteFreeNames(names);
                    return false;
                }
                continue;
            }
            if (WantsAsciiConversion(flags.transferMode, name))
            {
                unsigned long long converted = 0;
                if (!ConvertToUnixLineEndings(childLocal, converted, dirErr))
                {
                    Console::Instance().Error(Texts::LocalError(
                        "convert the line endings of", childLocal, dirErr));
                    mErrors++;
                    success = false;
                    if (AbortAfterFileError())
                    {
                        RemoteFreeNames(names);
                        return false;
                    }
                    continue;
                }
                size = converted;
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

    /*
     * '-preservetime=all': the directory itself gets the time of its remote
     * source once, after everything inside it was written (writing a file
     * updates the parent directory again, which is why this happens here and
     * not while the entries are copied).
     */
    if (success && flags.preserveTimeDirs && flags.preserveTime)
    {
        LscpAttrs attrs = {};
        std::string timeError;
        if (RemoteStat(remoteDir, &attrs, timeError) == LSCP_SFTP_STAT_EXISTS &&
            (attrs.flags & LSCP_ATTR_ACMODTIME) != 0 && attrs.mtime != 0)
        {
            SetFileTimes(localDir, attrs.atime != 0 ? attrs.atime : attrs.mtime,
                attrs.mtime);
        }
    }

    RemoteFreeNames(names);
    return success;
}

bool Script::UploadDirectoryRecursive(const std::string & localDir,
    const std::string & remoteDir, const TransferFlags & flags,
    unsigned long long & bytesTransferred, const std::string & relPrefix)
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

        const std::string childRel = relPrefix.empty() ? name :
            relPrefix + "/" + name;

        LocalFileInfo info = StatLocal(childLocal);
        if (info.isDir)
        {
            /* A directory is pruned only by an exclusion. */
            if (!flags.mask.empty() &&
                !flags.mask.Accepts(name, true, 0, 0, childRel))
            {
                continue;
            }
            if (!UploadDirectoryRecursive(childLocal, childRemote, flags,
                bytesTransferred, childRel))
            {
                success = false;
                if (AbortAfterFileError())
                {
                    return false;
                }
            }
            if (flags.deleteSource)
            {
                std::string removeError;
                RemoveLocalDirectory(childLocal, removeError);
            }
        }
        else if (info.isRegular)
        {
            if (!flags.mask.empty() &&
                !flags.mask.Accepts(name, false, info.size, info.mtime, childRel))
            {
                continue;
            }
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
            std::string uploadPath = childLocal;
            std::string asciiTemp;
            if (WantsAsciiConversion(flags.transferMode, name))
            {
                if (!MakeCRLFUploadCopy(childLocal, asciiTemp, error))
                {
                    Console::Instance().Error(Texts::LocalError(
                        "convert the line endings of", childLocal, error));
                    mErrors++;
                    success = false;
                    if (AbortAfterFileError())
                    {
                        return false;
                    }
                    continue;
                }
                uploadPath = asciiTemp;
            }
            Console::Instance().TransferStart(name, info.size);
            int result = RemotePut(uploadPath, childRemote, flags.permissions, 0,
                flags.preserveTime, &size, ProgressCallback, NULL, error,
                flags.append);
            Console::Instance().TransferEnd(result == 0);
            if (!asciiTemp.empty())
            {
                remove(asciiTemp.c_str());
            }
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(childLocal, error));
                mErrors++;
                success = false;
                if (AbortAfterFileError())
                {
                    return false;
                }
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

    /* '-preservetime=all' on an upload: see the download for the why. */
    if (success && flags.preserveTimeDirs && flags.preserveTime)
    {
        const LocalFileInfo info = StatLocal(localDir);
        if (info.exists && info.mtime != 0)
        {
            std::string timeError;
            if (RemoteSetTimes(remoteDir, info.atime != 0 ? info.atime : info.mtime,
                info.mtime, timeError) != 0 && !timeError.empty())
            {
                Console::Instance().Status("Could not set the timestamps of '" +
                    remoteDir + "': " + timeError);
            }
        }
    }
    return success;
}

/* Every rejection of a transfer switch says the same thing in the same
 * place, so that no path forgets to count the error. */
bool Script::SwitchError(const std::string & command, const std::string & hint)
{
    Console::Instance().Error(Texts::WrongArguments(command, hint));
    mErrors++;
    return false;
}

/*
 * The parameters of this run, expanded exactly as the upstream console
 * expands them, before the line is split into words.
 */
bool Script::ExpandParameters(const std::string & line, std::string & expanded)
{
    std::string error;
    if (ExpandScriptParameters(line, mParameters, expanded, error))
    {
        return true;
    }
    Console::Instance().Error(error);
    mErrors++;
    return false;
}

/*
 * 'synch' means 'synchronize' upstream; 'co' is refused because it could be
 * 'close' or 'command'. A name that matches nothing is reported by the
 * caller, which knows whether an unknown command or an unknown option is
 * the worse mistake.
 */
bool Script::ResolveAbbreviation(const std::vector<std::string> & names,
    const std::string & typed, const std::string & what,
    std::string & resolved)
{
    resolved.clear();
    std::string matches;
    const int index = FindPrefixedName(names, typed, matches);
    if (index == kNameAmbiguous)
    {
        Console::Instance().Error(Texts::WrongArguments(typed,
            "The " + what + " '" + typed + "' is ambiguous: it matches " +
            matches + ". Type them out in full."));
        mErrors++;
        return false;
    }
    if (index >= 0)
    {
        resolved = names[static_cast<size_t>(index)];
    }
    return true;
}

namespace {

/* The vocabularies an abbreviated name is resolved against. */
const std::vector<std::string> & OptionNames()
{
    static const std::vector<std::string> names = {
        "echo", "batch", "confirm", "resume", "transfer", "include",
        "exclude", "failonnomatch", "synchdelete", "reconnecttime",
        "editor", "editmaxsize"
    };
    return names;
}

const std::vector<std::string> & ToggleNames()
{
    static const std::vector<std::string> names = { "off", "on" };
    return names;
}

const std::vector<std::string> & BatchModeNames()
{
    static const std::vector<std::string> names = {
        "off", "on", "abort", "continue"
    };
    return names;
}

const std::vector<std::string> & TransferModeNames()
{
    static const std::vector<std::string> names = {
        "binary", "ascii", "automatic"
    };
    return names;
}

/*
 * A transfer mode, abbreviated the way upstream abbreviates everything else.
 * 'matches' receives the candidates when the text fits more than one mode, so
 * that the caller can say which ones it could have meant.
 */
bool ParseTransferModeName(const std::string & text, TransferMode & mode,
    std::string & matches)
{
    matches.clear();
    const std::string resolved = ResolvePrefixedName(TransferModeNames(),
        ToLower(text), matches);
    if (!matches.empty() || resolved.empty())
    {
        return false;
    }
    return ParseTransferMode(resolved, mode);
}

} // namespace

/*
 * Whether one file of this size is resumed. '-resumesupport=' is the
 * authoritative switch when it is given: 'off' never resumes, 'on' always
 * does, and a size in kilobytes resumes only the files at least that big
 * (the upstream 'smart' setting). Without it, '-resume' and
 * 'option resume' decide, as before.
 *
 * An ASCII transfer is never resumed by the size threshold: converting line
 * endings changes the byte count, so there is no offset to continue from.
 */
bool Script::Resumes(const TransferFlags & flags, unsigned long long size) const
{
    switch (flags.resumeSupport)
    {
        case TransferFlags::ResumeOff:
            return false;
        case TransferFlags::ResumeOn:
            return true;
        case TransferFlags::ResumeSmart:
            return flags.transferMode == TransferMode::Binary &&
                size >= flags.resumeThreshold;
        case TransferFlags::ResumeUnset:
        default:
            break;
    }
    return flags.resume || mResume;
}

/*
 * Reads the switches of 'get' and 'put' into 'flags' and collects the file
 * names in 'paths'. Anything it does not understand is reported here, before
 * a connection is opened or a file is touched, so that a switch is never
 * silently ignored: a script that asks for a setting this client cannot
 * honour must fail rather than transfer something else.
 */
bool Script::ParseTransferSwitches(const std::string & command, bool uploads,
    const std::vector<std::string> & args, TransferFlags & flags,
    std::vector<std::string> & paths)
{
    /* Resuming needs stable byte offsets, so a switch that asks for it
     * outright cannot be combined with an ASCII/automatic transfer,
     * whichever way the mode was set. */
    bool resumeForced = mResume;
    TransferMode mode = mTransferMode;

    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & a = args[i];
        if (a.empty() || a[0] != '-')
        {
            paths.push_back(a);
            continue;
        }
        if (a == "-delete" || a == "-r" || a == "-resume" ||
            a == "-neweronly" || a == "-noneweronly" || a == "-preservetime" ||
            a == "-nopreservetime" || a == "-append" || a == "-nopermissions")
        {
            if (a == "-delete")
            {
                flags.deleteSource = true;
            }
            else if (a == "-neweronly")
            {
                flags.newerOnly = true;
            }
            else if (a == "-noneweronly")
            {
                flags.newerOnly = false;
            }
            else if (a == "-preservetime")
            {
                flags.preserveTime = true;
            }
            else if (a == "-nopreservetime")
            {
                flags.preserveTime = false;
                flags.preserveTimeDirs = false;
            }
            else if (a == "-append")
            {
                flags.append = true;
            }
            else if (a == "-resume")
            {
                flags.resume = true;
                resumeForced = true;
            }
            else if (a == "-nopermissions")
            {
                if (!uploads)
                {
                    return SwitchError(command,
                        "The '-nopermissions' switch applies to uploads only.");
                }
                flags.permissions = -1;
            }
            continue;
        }
        if (StartsWith(a, "-preservetime="))
        {
            /* Upstream takes 'all' to preserve the time of the transferred
             * directories as well as of their files. */
            const std::string value = ToLower(a.substr(strlen("-preservetime=")));
            if (value != "all")
            {
                return SwitchError(command, "The only value of "
                    "'-preservetime' is 'all' (directories are timed too); "
                    "use '-nopreservetime' to keep no timestamps.");
            }
            flags.preserveTime = true;
            flags.preserveTimeDirs = true;
            continue;
        }
        if (StartsWith(a, "-transfer="))
        {
            std::string matches;
            if (!ParseTransferModeName(a.substr(strlen("-transfer=")), mode,
                matches))
            {
                return SwitchError(command, matches.empty() ?
                    "The value of '-transfer' must be binary, ascii or "
                    "automatic." :
                    "The value '" + a.substr(strlen("-transfer=")) + "' of "
                    "'-transfer' is ambiguous: it matches " + matches + ".");
            }
            flags.transferMode = mode;
            continue;
        }
        if (StartsWith(a, "-filemask="))
        {
            std::string maskError;
            if (!flags.mask.Parse(a.substr(strlen("-filemask=")), maskError))
            {
                return SwitchError(command, maskError);
            }
            flags.maskGiven = true;
            continue;
        }
        if (StartsWith(a, "-permissions="))
        {
            if (!uploads)
            {
                return SwitchError(command, "The '-permissions' switch applies "
                    "to uploads only.");
            }
            unsigned long permissions = 0;
            if (!ParseOctalPermissions(a.substr(strlen("-permissions=")),
                permissions))
            {
                return SwitchError(command, "The value of '-permissions' must "
                    "be an octal number from 0 through 7777.");
            }
            flags.permissions = static_cast<int>(permissions);
            continue;
        }
        if (StartsWith(a, "-speed="))
        {
            /* Like upstream, the value is a number of kibibytes per second
             * and an empty or negative one means no limit at all. */
            const std::string value = a.substr(strlen("-speed="));
            if (value.empty())
            {
                flags.speedKBytesPerSecond = 0;
                continue;
            }
            char * end = NULL;
            const long limit = strtol(value.c_str(), &end, 10);
            if (end == NULL || *end != '\0' || limit < 0)
            {
                return SwitchError(command, "The value of '-speed' must be a "
                    "whole number of kilobytes per second (0 removes the "
                    "limit).");
            }
            flags.speedKBytesPerSecond = static_cast<unsigned>(limit);
            continue;
        }
        if (StartsWith(a, "-resumesupport="))
        {
            const std::string value = ToLower(a.substr(strlen("-resumesupport=")));
            if (value == "off" || value == "on")
            {
                flags.resumeSupport = value == "off" ? TransferFlags::ResumeOff :
                    TransferFlags::ResumeOn;
            }
            else
            {
                char * end = NULL;
                const long threshold = strtol(value.c_str(), &end, 10);
                if (end == NULL || *end != '\0' || threshold < 0)
                {
                    return SwitchError(command, "The value of "
                        "'-resumesupport' must be on, off or a number of "
                        "kilobytes a file must reach to be resumed.");
                }
                flags.resumeSupport = TransferFlags::ResumeSmart;
                flags.resumeThreshold =
                    static_cast<unsigned long long>(threshold) * 1024ULL;
            }
            resumeForced = flags.resumeSupport == TransferFlags::ResumeOn;
            continue;
        }
        return SwitchError(command, "Unsupported switch '" + a + "'; it will "
            "not be ignored. Prefix a filename beginning with '-' with './'.");
    }

    if (flags.append && flags.resume)
    {
        return SwitchError(command, "'-append' and '-resume' cannot be used "
            "together: one continues at the end of the destination, the "
            "other at a shared offset in it.");
    }
    if (resumeForced && mode != TransferMode::Binary)
    {
        return SwitchError(command, "Resuming a transfer is binary only: an "
            "ASCII transfer converts line endings, so byte offsets do not "
            "survive.");
    }
    return true;
}

/* The mask a transfer uses: '-filemask=' overrides the persistent
 * 'option include/exclude' mask, and says so. */
void Script::ResolveTransferMask(TransferFlags & flags) const
{
    if (flags.maskGiven)
    {
        if (mOptionMaskUsed)
        {
            Console::Instance().Status("The -filemask switch overrides the "
                "include/exclude option mask for this command.");
        }
        return;
    }
    flags.mask = mOptionMask;
}

bool Script::ListLocal(const std::vector<std::string> & args)
{
    if (args.size() > 1)
    {
        Console::Instance().Error(Texts::WrongArguments("lls", "Syntax: lls [<directory>[/<mask>]]"));
        mErrors++;
        return false;
    }
    std::string error;
    std::string path = args.empty() ? GetLocalDirectory(error) : ExpandTilde(args[0]);
    std::vector<std::string> paths;
    if (StatLocal(path).isDir)
    {
        std::vector<std::string> names;
        if (!ListLocalDirectory(path, names, error))
        {
            Console::Instance().Error(Texts::CannotReadDirectory(path, error));
            mErrors++;
            return false;
        }
        for (const std::string & name : names)
        {
            paths.push_back(JoinLocalPath(path, name));
        }
    }
    else if (!ExpandLocalWildcard(path, paths, error) ||
        (!HasWildcard(path) && paths.empty()))
    {
        Console::Instance().Error(Texts::LocalError("list", path,
            error.empty() ? "No such file or directory." : error));
        mErrors++;
        return false;
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string & entry : paths)
    {
        Console::Instance().WriteLine(LocalFileName(entry));
    }
    return true;
}

bool Script::GetFiles(const std::vector<std::string> & args)
{
    TransferFlags flags;
    flags.transferMode = mTransferMode;
    std::vector<std::string> paths;
    if (!ParseTransferSwitches("get", false, args, flags, paths))
    {
        return false;
    }
    if (!RequireConnection())
    {
        return false;
    }

    if (paths.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("get",
            "Syntax: get [switches] <remotefile(s)> [<localpath>]"));
        mErrors++;
        return false;
    }

    /* Only the native transport can write at a position of the local file
     * that is not the offset it read on the remote one. */
    if (flags.append && !UseNativeSftp(mSession.Options().protocol))
    {
        return SwitchError("get", "'-append' needs the native SFTP "
            "transport; the transfer backends cannot position a write.");
    }
    ResolveTransferMask(flags);
    SpeedLimitForThisCommand limiter(flags.speedKBytesPerSecond, &mSpeedLimit);

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
            if (HasWildcard(sourcePatterns[i]))
            {
                Console::Instance().Status("No file matching '" +
                    sourcePatterns[i] + "' was found.");
                if (mFailOnNoMatch)
                {
                    mErrors++;
                }
                continue;
            }
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

            /* The file mask applies to the selected names too; a
             * directory itself is filtered by what is inside it. */
            if (!flags.mask.empty() && !isDir &&
                !flags.mask.Accepts(RemoteFileName(sources[n]), false,
                    (sourceAttrs.flags & LSCP_ATTR_SIZE) != 0 ? sourceAttrs.size : 0,
                    (sourceAttrs.flags & LSCP_ATTR_ACMODTIME) != 0 ? sourceAttrs.mtime : 0,
                    RemoteFileName(sources[n])))
            {
                continue;
            }

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

            unsigned long long totalSize = 0;
            if ((sourceAttrs.flags & LSCP_ATTR_SIZE) != 0)
            {
                totalSize = sourceAttrs.size;
            }
            /* Asking before overwriting is pointless when the file is only
             * continued, and '-append' never destroys anything. */
            const bool resumeThis = Resumes(flags, totalSize);
            if (mConfirm && localInfo.exists && !resumeThis && !flags.append)
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

            unsigned long long resumeOffset = 0;
            if (resumeThis && localInfo.exists && localInfo.isRegular && localInfo.size < totalSize)
            {
                resumeOffset = localInfo.size;
                Console::Instance().Status("Resuming download of '" + RemoteFileName(sources[n]) +
                    "' from byte " + std::to_string(resumeOffset));
            }
            unsigned long long size = 0;
            Console::Instance().TransferStart(RemoteFileName(sources[n]), totalSize);
            int result = RemoteGet(sources[n], localPath, resumeOffset,
                flags.preserveTime, &size, ProgressCallback, NULL, error,
                flags.append);
            Console::Instance().TransferEnd(result == 0);
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(sources[n], error));
                mErrors++;
                if (AbortAfterFileError())
                {
                    return false;
                }
                continue;
            }
            if (WantsAsciiConversion(flags.transferMode, RemoteFileName(sources[n])))
            {
                unsigned long long converted = 0;
                if (!ConvertToUnixLineEndings(localPath, converted, error))
                {
                    Console::Instance().Error(Texts::LocalError(
                        "convert the line endings of", localPath, error));
                    mErrors++;
                    if (AbortAfterFileError())
                    {
                        return false;
                    }
                    continue;
                }
                size = converted;
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
    TransferFlags flags;
    flags.transferMode = mTransferMode;
    std::vector<std::string> paths;
    if (!ParseTransferSwitches("put", true, args, flags, paths))
    {
        return false;
    }
    if (!RequireConnection())
    {
        return false;
    }

    if (paths.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("put",
            "Syntax: put [switches] <localfile(s)> [<remotepath>]"));
        mErrors++;
        return false;
    }

    if (flags.append && !UseNativeSftp(mSession.Options().protocol))
    {
        return SwitchError("put", "'-append' needs the native SFTP "
            "transport; the transfer backends cannot position a write.");
    }
    ResolveTransferMask(flags);
    SpeedLimitForThisCommand limiter(flags.speedKBytesPerSecond, &mSpeedLimit);

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
            if (HasWildcard(sourcePatterns[i]))
            {
                Console::Instance().Status("No file matching '" +
                    sourcePatterns[i] + "' was found.");
                if (mFailOnNoMatch)
                {
                    mErrors++;
                }
                continue;
            }
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
            /* The file mask applies to the selected names too; a
             * directory is filtered by what is inside it. */
            if (!flags.mask.empty() && !info.isDir &&
                !flags.mask.Accepts(LocalFileName(sources[n]), false,
                    info.size, info.mtime, LocalFileName(sources[n])))
            {
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
            const bool resumeThis = Resumes(flags, info.size);
            if (mConfirm && remoteExists && !resumeThis && !flags.append)
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
            if (resumeThis && remoteExists && remoteInfo.size < info.size)
            {
                resumeOffset = remoteInfo.size;
                Console::Instance().Status("Resuming upload of '" + LocalFileName(sources[n]) +
                    "' from byte " + std::to_string(resumeOffset));
            }
            unsigned long long size = 0;
            /* ASCII uploads convert a temporary copy, so the local file
             * is never modified. */
            std::string uploadPath = sources[n];
            std::string asciiTemp;
            const bool convertAscii =
                WantsAsciiConversion(flags.transferMode, LocalFileName(sources[n]));
            if (convertAscii)
            {
                if (!MakeCRLFUploadCopy(sources[n], asciiTemp, error))
                {
                    Console::Instance().Error(Texts::LocalError(
                        "convert the line endings of", sources[n], error));
                    mErrors++;
                    if (AbortAfterFileError())
                    {
                        return false;
                    }
                    continue;
                }
                uploadPath = asciiTemp;
            }
            Console::Instance().TransferStart(LocalFileName(sources[n]), info.size);
            int result = RemotePut(uploadPath, remotePath, flags.permissions,
                resumeOffset, flags.preserveTime, &size, ProgressCallback, NULL,
                error, flags.append);
            Console::Instance().TransferEnd(result == 0);
            if (!asciiTemp.empty())
            {
                remove(asciiTemp.c_str());
            }
            if (result != 0)
            {
                Console::Instance().Error(Texts::TransferFailed(sources[n], error));
                mErrors++;
                if (AbortAfterFileError())
                {
                    return false;
                }
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

/* ------------------------------------------------------------------ *
 * Synchronization                                                     *
 * ------------------------------------------------------------------ */

namespace {

/* The local side of a synchronization. */
class LocalSyncFileSystem : public SyncFileSystem
{
public:
    virtual bool ListDirectory(const std::string & path,
        std::vector<SyncEntry> & entries, std::string & error)
    {
        std::vector<std::string> names;
        if (!ListLocalDirectory(path, names, error))
        {
            return false;
        }
        for (size_t i = 0; i < names.size(); i++)
        {
            const LocalFileInfo info = StatLocal(JoinLocalPath(path, names[i]));
            SyncEntry entry;
            entry.name = names[i];
            entry.isDirectory = info.isDir;
            entry.size = info.size;
            entry.mtime = info.mtime;
            entry.permissions = static_cast<unsigned long>(info.permissions);
            entries.push_back(entry);
        }
        return true;
    }

    virtual const char * Label() const { return "local"; }
};

/* The remote side: whatever protocol this session speaks. */
class RemoteSyncFileSystem : public SyncFileSystem
{
public:
    explicit RemoteSyncFileSystem(Script & script) : mScript(script) {}

    virtual bool ListDirectory(const std::string & path,
        std::vector<SyncEntry> & entries, std::string & error)
    {
        return mScript.ListRemoteEntries(path, entries, error);
    }

    virtual const char * Label() const { return "remote"; }

private:
    Script & mScript;
};

std::string DescribeTime(unsigned long value)
{
    if (value == 0)
    {
        return "unknown";
    }
    std::tm parts;
    if (!LocalTime(static_cast<std::time_t>(value), parts))
    {
        return "unknown";
    }
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts);
    return std::string(buffer);
}

/* True for a directory that is the root of a file system: deleting
 * "obsolete" entries there is far more likely to be a mistake than a
 * synchronization, so 'synchronize -delete' refuses it. */
bool IsFileSystemRoot(const std::string & path)
{
    std::string trimmed = path;
    while (trimmed.size() > 1 &&
        (trimmed[trimmed.size() - 1] == '/' || trimmed[trimmed.size() - 1] == '\\'))
    {
        trimmed.erase(trimmed.size() - 1);
    }
#if defined(_WIN32)
    if (trimmed.size() == 2 && trimmed[1] == ':')
    {
        return true; /* C: */
    }
#endif
    if (trimmed.empty())
    {
        return true;
    }
    return false;
}

} // namespace

bool Script::ListRemoteEntries(const std::string & path,
    std::vector<SyncEntry> & entries, std::string & error)
{
    LscpNames * names = NULL;
    if (!RemoteListDir(path, names, error))
    {
        return false;
    }
    for (int i = 0; i < names->count; i++)
    {
        const char * raw = names->names[i].filename;
        if (raw == NULL)
        {
            continue;
        }
        const std::string name(raw);
        if (name == "." || name == "..")
        {
            continue;
        }
        SyncEntry entry;
        entry.name = name;
        const LscpAttrs & attrs = names->names[i].attrs;
        if ((attrs.flags & LSCP_ATTR_PERMISSIONS) != 0)
        {
            entry.isDirectory = (attrs.permissions & 0040000) != 0;
            entry.permissions = attrs.permissions;
        }
        if ((attrs.flags & LSCP_ATTR_SIZE) != 0)
        {
            entry.size = attrs.size;
        }
        if ((attrs.flags & LSCP_ATTR_ACMODTIME) != 0)
        {
            entry.mtime = attrs.mtime;
        }
        /* A backend that does not report the permissions of a listing
         * (curl) still has to be able to tell a directory from a file. */
        if ((attrs.flags & LSCP_ATTR_PERMISSIONS) == 0)
        {
            LscpAttrs child = {};
            std::string statError;
            if (RemoteStat(JoinRemotePath(path, name), &child, statError) ==
                LSCP_SFTP_STAT_EXISTS)
            {
                entry.isDirectory = (child.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
                    (child.permissions & 0040000) != 0;
                if ((child.flags & LSCP_ATTR_PERMISSIONS) != 0)
                {
                    entry.permissions = child.permissions;
                }
                if ((child.flags & LSCP_ATTR_SIZE) != 0)
                {
                    entry.size = child.size;
                }
                if ((child.flags & LSCP_ATTR_ACMODTIME) != 0)
                {
                    entry.mtime = child.mtime;
                }
            }
        }
        entries.push_back(entry);
    }
    RemoteFreeNames(names);
    return true;
}

bool Script::ParseSyncArguments(const std::vector<std::string> & args,
    const std::string & command, SyncOptions & options,
    std::vector<std::string> & dirs, bool & preview, int & interval,
    int & count, bool & continueOnError, bool modeOptional,
    std::string & error)
{
    error.clear();
    preview = false;
    interval = 10;
    count = 0;
    continueOnError = false;
    bool modeGiven = false;
    /* 'option synchdelete on' is the persistent form of '-delete'. */
    options.deleteObsolete = mSyncDeleteDefault;
    options.transferMode = mTransferMode;
    /* 'keepuptodate' has no direction word: it always watches the
     * remote side and updates the local one. */
    if (modeOptional)
    {
        options.mode = SyncMode::Local;
        modeGiven = true;
    }

    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & a = args[i];
        const std::string lower = ToLower(a);
        if (i == 0 && !modeOptional &&
            (lower == "local" || lower == "remote" || lower == "both"))
        {
            options.mode = lower == "local" ? SyncMode::Local :
                lower == "remote" ? SyncMode::Remote : SyncMode::Both;
            modeGiven = true;
        }
        else if (lower == "-delete")
        {
            options.deleteObsolete = true;
        }
        else if (lower == "-nodelete")
        {
            options.deleteObsolete = false;
        }
        else if (lower == "-mirror")
        {
            options.mirror = true;
        }
        else if (lower == "-continue")
        {
            continueOnError = true;
        }
        else if (StartsWith(lower, "-transfer="))
        {
            TransferMode mode;
            if (!ParseTransferMode(lower.substr(strlen("-transfer=")), mode))
            {
                error = "The value of '-transfer' must be binary, ascii or "
                    "automatic.";
                return false;
            }
            options.transferMode = mode;
        }
        else if (lower == "-preview" || lower == "-dryrun")
        {
            preview = true;
        }
        else if (lower == "-nonrecursive" || lower == "-nor")
        {
            options.recursive = false;
        }
        else if (StartsWith(lower, "-criteria="))
        {
            const std::string value = lower.substr(strlen("-criteria="));
            if (value == "time")
            {
                options.criteria = SyncCriteria::Time;
            }
            else if (value == "size")
            {
                options.criteria = SyncCriteria::Size;
            }
            else if (value == "either")
            {
                options.criteria = SyncCriteria::Either;
            }
            else if (value == "none")
            {
                options.criteria = SyncCriteria::None;
            }
            else
            {
                error = "Unsupported criteria '" + value +
                    "' (use time, size, either or none).";
                return false;
            }
        }
        else if (StartsWith(lower, "-filemask="))
        {
            const std::string value = a.substr(strlen("-filemask="));
            std::string maskError;
            if (!options.mask.Parse(value, maskError))
            {
                error = maskError;
                return false;
            }
        }
        else if (StartsWith(lower, "-interval="))
        {
            const int value = atoi(a.substr(strlen("-interval=")).c_str());
            if (value < 1 || value > 86400)
            {
                error = "The interval must be between 1 and 86400 seconds.";
                return false;
            }
            interval = value;
        }
        else if (StartsWith(lower, "-count="))
        {
            const int value = atoi(a.substr(strlen("-count=")).c_str());
            if (value < 1 || value > 1000000)
            {
                error = "The count must be a positive number of passes.";
                return false;
            }
            count = value;
        }
        else if (!a.empty() && a[0] == '-')
        {
            error = "Unsupported switch '" + a + "'; it will not be ignored.";
            return false;
        }
        else
        {
            dirs.push_back(a);
        }
    }

    if (!modeGiven || dirs.size() > 2)
    {
        error = "Syntax: " + command +
            (modeOptional ? std::string(" [LOCAL [REMOTE]]") :
                std::string(" local|remote|both [LOCAL [REMOTE]]")) +
            " [-delete] [-nodelete] [-mirror] [-preview] [-nonrecursive] "
            "[-criteria=time|size|either|none] [-filemask=MASK] "
            "[-transfer=binary|ascii|automatic] [-interval=SECS] [-count=N] "
            "[-continue].";
        return false;
    }
    if (options.mirror && options.mode == SyncMode::Both)
    {
        error = "'-mirror' cannot be combined with the 'both' direction.";
        return false;
    }
    return true;
}

bool Script::BuildSynchronizationPlan(SyncOptions & options,
    const std::string & localDir, const std::string & remoteDir, SyncPlan & plan,
    SyncState * state, std::string & error)
{
    options.state = state;
    LocalSyncFileSystem localFs;
    RemoteSyncFileSystem remoteFs(*this);
    return PlanSynchronization(localFs, remoteFs, localDir, remoteDir,
        options, plan, error);
}

void Script::PrintSynchronizationPlan(const SyncPlan & plan) const
{
    Console & console = Console::Instance();
    console.Write(std::string("SYNCPLAN local=") + plan.localRoot +
        "\tremote=" + plan.remoteRoot +
        "\tmode=" + SyncModeName(plan.mode) +
        "\tcriteria=" + SyncCriteriaName(plan.criteria) +
        "\tmask=" + plan.mask.Describe() + "\n");
    for (size_t i = 0; i < plan.items.size(); i++)
    {
        const SyncItem & item = plan.items[i];
        if (item.action == SyncAction::None)
        {
            continue;
        }
        std::string detail = item.reason;
        if (item.localTime != 0 || item.remoteTime != 0)
        {
            detail += " (local " + DescribeTime(item.localTime) +
                ", remote " + DescribeTime(item.remoteTime) + ")";
        }
        console.Write(std::string(SyncActionName(item.action)) + "\t" + item.path +
            "\t" + detail + "\n");
    }
    char summary[256];
    snprintf(summary, sizeof(summary),
        "SYNCSUMMARY download=%d upload=%d delete-local=%d delete-remote=%d "
        "conflict=%d unchanged=%d bytes=%llu\n",
        plan.Count(SyncAction::Download), plan.Count(SyncAction::Upload),
        plan.Count(SyncAction::DeleteLocal), plan.Count(SyncAction::DeleteRemote),
        plan.Count(SyncAction::Conflict), plan.Count(SyncAction::None),
        plan.Bytes(SyncAction::Download) + plan.Bytes(SyncAction::Upload));
    console.Write(std::string(summary));
}

void Script::RecordSyncState(SyncState * state, const std::string & relative,
    const std::string & localPath, const std::string & remotePath)
{
    if (state == NULL)
    {
        return;
    }
    SyncStateEntry entry;
    const LocalFileInfo info = StatLocal(localPath);
    if (!info.exists)
    {
        return;
    }
    entry.localTime = info.mtime;
    entry.size = info.size;
    LscpAttrs attrs = {};
    std::string error;
    if (RemoteStat(remotePath, &attrs, error) == LSCP_SFTP_STAT_EXISTS)
    {
        if ((attrs.flags & LSCP_ATTR_ACMODTIME) != 0)
        {
            entry.remoteTime = attrs.mtime;
        }
        if ((attrs.flags & LSCP_ATTR_SIZE) != 0)
        {
            entry.size = attrs.size;
        }
    }
    state->Update(relative, entry);
}

bool Script::ExecuteSynchronizationPlan(SyncPlan & plan, const SyncOptions & options,
    bool allowDelete, unsigned long long & bytesTransferred)
{
    Console & console = Console::Instance();
    bool success = true;
    bool deleteAllowed = allowDelete && options.deleteObsolete;

    if (deleteAllowed && plan.HasConflicts())
    {
        console.Error("The plan has " +
            std::to_string(plan.Count(SyncAction::Conflict)) +
            " conflict(s); nothing was deleted. Resolve the conflicts (or "
            "synchronize without -delete) and run the synchronization again.");
        deleteAllowed = false;
        mErrors++;
        success = false;
    }

    for (size_t i = 0; i < plan.items.size(); i++)
    {
        const SyncItem & item = plan.items[i];
        const std::string localPath =
            LocalPathForRelative(plan.localRoot, item.path);
        const std::string remotePath =
            RemotePathForRelative(plan.remoteRoot, item.path);
        std::string error;

        switch (item.action)
        {
            case SyncAction::None:
                RecordSyncState(options.state, item.path, localPath, remotePath);
                break;

            case SyncAction::Conflict:
                console.Error("Conflict: '" + item.path + "' " + item.reason +
                    ", so it was left unchanged.");
                mErrors++;
                success = false;
                break;

            case SyncAction::Upload:
                if (item.isDirectory)
                {
                    if (!CreateRemoteDirectoryRecursive(remotePath, error))
                    {
                        console.Error(Texts::RemoteError("create directory",
                            remotePath, error));
                        mErrors++;
                        success = false;
                    }
                    else
                    {
                        console.Status("Created remote directory '" + remotePath + "'.");
                    }
                    break;
                }
                if (!CreateRemoteDirectoryRecursive(RemoteDirName(remotePath), error))
                {
                    console.Error(Texts::RemoteError("create directory",
                        RemoteDirName(remotePath), error));
                    mErrors++;
                    success = false;
                    break;
                }
                {
                    console.TransferStart(item.path, item.localSize);
                    unsigned long long size = 0;
                    const int result = RemotePutMode(localPath, remotePath,
                        options.transferMode, &size, error);
                    console.TransferEnd(result == 0);
                    if (result != 0)
                    {
                        console.Error(Texts::TransferFailed(remotePath, error));
                        mErrors++;
                        success = false;
                        break;
                    }
                    bytesTransferred += size;
                    console.Status("Uploaded " + Texts::FileTransferred(remotePath, size));
                    RecordSyncState(options.state, item.path, localPath, remotePath);
                }
                break;

            case SyncAction::Download:
                if (item.isDirectory)
                {
                    if (!CreateLocalDirectoryRecursive(localPath, error))
                    {
                        console.Error(Texts::LocalError("create directory",
                            localPath, error));
                        mErrors++;
                        success = false;
                    }
                    else
                    {
                        console.Status("Created local directory '" + localPath + "'.");
                    }
                    break;
                }
                if (!CreateLocalDirectoryRecursive(LocalDirName(localPath), error))
                {
                    console.Error(Texts::LocalError("create directory",
                        LocalDirName(localPath), error));
                    mErrors++;
                    success = false;
                    break;
                }
                {
                    console.TransferStart(item.path, item.remoteSize);
                    unsigned long long size = 0;
                    const int result = RemoteGetMode(remotePath, localPath,
                        options.transferMode, &size, error);
                    console.TransferEnd(result == 0);
                    if (result != 0)
                    {
                        console.Error(Texts::TransferFailed(remotePath, error));
                        mErrors++;
                        success = false;
                        break;
                    }
                    bytesTransferred += size;
                    console.Status("Downloaded " + Texts::FileTransferred(localPath, size));
                    RecordSyncState(options.state, item.path, localPath, remotePath);
                }
                break;

            case SyncAction::DeleteLocal:
                if (!deleteAllowed)
                {
                    if (options.deleteObsolete)
                    {
                        console.Status("Kept '" + localPath + "'.");
                    }
                    break;
                }
                if (!ConfirmObsolete("local", localPath, item.isDirectory))
                {
                    success = false;
                    break;
                }
                if (!RemoveLocalItemRecursive(localPath))
                {
                    success = false;
                    break;
                }
                if (options.state != NULL)
                {
                    options.state->Remove(item.path);
                }
                break;

            case SyncAction::DeleteRemote:
                if (!deleteAllowed)
                {
                    if (options.deleteObsolete)
                    {
                        console.Status("Kept '" + remotePath + "'.");
                    }
                    break;
                }
                if (!ConfirmObsolete("remote", remotePath, item.isDirectory))
                {
                    success = false;
                    break;
                }
                if (!RemoveRemoteItemRecursive(remotePath))
                {
                    success = false;
                    break;
                }
                if (options.state != NULL)
                {
                    options.state->Remove(item.path);
                }
                break;
        }
    }
    return success;
}

bool Script::SynchronizeDirectories(const std::vector<std::string> & args)
{
    Console & console = Console::Instance();
    SyncOptions options;
    std::vector<std::string> dirs;
    bool preview = false;
    int interval = 10;
    int count = 0;
    bool continueOnError = false;
    std::string parseError;
    if (!ParseSyncArguments(args, "synchronize", options, dirs, preview,
        interval, count, continueOnError, false, parseError))
    {
        console.Error(Texts::WrongArguments("synchronize", parseError));
        mErrors++;
        return false;
    }
    if (!RequireConnection())
    {
        return false;
    }

    std::string error;
    std::string localDir = dirs.size() > 0 ? ExpandTilde(dirs[0]) : GetLocalDirectory(error);
    std::string remoteDir = dirs.size() > 1 ?
        ResolveRemotePath(mRemoteDirectory, dirs[1]) : mRemoteDirectory;
    if (localDir.empty())
    {
        console.Error("Cannot determine local directory: " + error);
        mErrors++;
        return false;
    }
    if (remoteDir.empty())
    {
        remoteDir = "/";
    }

    /* The memory of the previous pass: what makes 'both' able to tell a
     * conflict from a change, and a deletion from a new file. */
    SyncState state;
    const std::string stateKey = SyncState::MakeKey(
        mSession.IsOpen() ? mSession.Options().hostName : std::string(),
        localDir, remoteDir);
    const std::string statePath = SyncState::DefaultPath(stateKey);
    std::string stateError;
    if (!state.Load(statePath, stateError))
    {
        console.Error(stateError);
        mErrors++;
        return false;
    }
    const bool firstPass = state.Empty();

    SyncPlan plan;
    if (!BuildSynchronizationPlan(options, localDir, remoteDir, plan, &state, error))
    {
        console.Error(error);
        mErrors++;
        return false;
    }

    if (preview)
    {
        PrintSynchronizationPlan(plan);
        return true;
    }

    if (options.deleteObsolete &&
        (IsFileSystemRoot(localDir) || IsFileSystemRoot(remoteDir)))
    {
        console.Error("Refusing to delete obsolete entries directly in the root "
            "directory '" +
            (IsFileSystemRoot(localDir) ? localDir : remoteDir) +
            "'. Synchronize a subdirectory instead.");
        mErrors++;
        return false;
    }
    if (options.mode == SyncMode::Both && options.deleteObsolete && firstPass)
    {
        /* Without a previous pass there is no way to tell a file that was
         * deleted on one side from a file that was created on the other,
         * so this pass only transfers and records what it saw. */
        console.Status("No previous synchronization of these directories is on "
            "record: this pass transfers files but deletes nothing. Run it "
            "again to let deletions follow.");
    }

    console.Status("Synchronizing (" + std::string(SyncModeName(options.mode)) +
        ") '" + localDir + "' and '" + remoteDir + "'...");

    unsigned long long bytes = 0;
    bool ok = ExecuteSynchronizationPlan(plan, options, true, bytes);

    if (!statePath.empty())
    {
        std::string saveError;
        if (!state.Save(statePath, saveError))
        {
            console.Error(saveError);
            mErrors++;
            ok = false;
        }
    }
    console.Status(ok ? "Synchronization completed." : "Synchronization failed.");
    return ok;
}

bool Script::KeepUpToDate(const std::vector<std::string> & args)
{
    Console & console = Console::Instance();
    SyncOptions options;
    std::vector<std::string> dirs;
    bool preview = false;
    int interval = 10;
    int count = 0;
    bool continueOnError = false;
    std::string parseError;
    if (!ParseSyncArguments(args, "keepuptodate", options, dirs, preview,
        interval, count, continueOnError, true, parseError))
    {
        console.Error(Texts::WrongArguments("keepuptodate", parseError));
        mErrors++;
        return false;
    }
    if (!RequireConnection())
    {
        return false;
    }
    if (count == 0 && !console.IsInteractive())
    {
        console.Error(Texts::WrongArguments("keepuptodate",
            "It would run until it is interrupted; give -count=<passes> to run "
            "a fixed number of passes."));
        mErrors++;
        return false;
    }

    std::string error;
    std::string localDir = dirs.size() > 0 ? ExpandTilde(dirs[0]) : GetLocalDirectory(error);
    std::string remoteDir = dirs.size() > 1 ?
        ResolveRemotePath(mRemoteDirectory, dirs[1]) : mRemoteDirectory;
    if (localDir.empty())
    {
        console.Error("Cannot determine local directory: " + error);
        mErrors++;
        return false;
    }
    if (remoteDir.empty())
    {
        remoteDir = "/";
    }
    if (options.deleteObsolete &&
        (IsFileSystemRoot(localDir) || IsFileSystemRoot(remoteDir)))
    {
        console.Error("Refusing to delete obsolete entries directly in the root "
            "directory; synchronize a subdirectory instead.");
        mErrors++;
        return false;
    }

    SyncState state;
    const std::string statePath = SyncState::DefaultPath(SyncState::MakeKey(
        mSession.IsOpen() ? mSession.Options().hostName : std::string(),
        localDir, remoteDir));
    std::string stateError;
    if (!state.Load(statePath, stateError))
    {
        console.Error(stateError);
        mErrors++;
        return false;
    }

    InstallInterruptHandler();
    console.Status("Watching '" + localDir + "' and '" + remoteDir + "' every " +
        std::to_string(interval) + " seconds. Press Ctrl-C to stop.");

    bool ok = true;
    for (int pass = 1; count == 0 || pass <= count; pass++)
    {
        if (Interrupted())
        {
            console.Status("Stopped watching.");
            break;
        }
        if (!mSession.IsOpen() || !mSession.IsAlive())
        {
            console.Error(Texts::NotConnected());
            mErrors++;
            ok = false;
            break;
        }

        SyncPlan plan;
        if (!BuildSynchronizationPlan(options, localDir, remoteDir, plan, &state, error))
        {
            console.Error(error);
            mErrors++;
            ok = false;
        }
        else if (preview)
        {
            PrintSynchronizationPlan(plan);
        }
        else if (plan.Count(SyncAction::Upload) == 0 &&
            plan.Count(SyncAction::Download) == 0 &&
            plan.Count(SyncAction::DeleteLocal) == 0 &&
            plan.Count(SyncAction::DeleteRemote) == 0 &&
            plan.Count(SyncAction::Conflict) == 0)
        {
            console.Status("Pass " + std::to_string(pass) +
                ": nothing to do (every file is up to date).");
        }
        else
        {
            unsigned long long bytes = 0;
            if (!ExecuteSynchronizationPlan(plan, options, true, bytes))
            {
                ok = false;
                if (!continueOnError)
                {
                    break;
                }
            }
            if (!statePath.empty())
            {
                std::string saveError;
                if (!state.Save(statePath, saveError))
                {
                    console.Error(saveError);
                    mErrors++;
                    ok = false;
                }
            }
        }

        if (count != 0 && pass == count)
        {
            break;
        }
        if (Interrupted())
        {
            console.Status("Stopped watching.");
            break;
        }
        /* Sleep in small steps so that Ctrl-C ends the wait. */
        for (int waited = 0; waited < interval * 4; waited++)
        {
            if (Interrupted())
            {
                break;
            }
            SleepMilliseconds(250);
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * Remote editing                                                      *
 * ------------------------------------------------------------------ */

namespace {

/* The open connection seen through the interface Editor needs. */
class ScriptFileAccess : public RemoteFileAccess
{
public:
    explicit ScriptFileAccess(Script & script) : mScript(script) {}

    virtual int Stat(const std::string & path, LscpAttrs & attrs, std::string & error)
    {
        return mScript.StatForEdit(path, attrs, error);
    }

    virtual int Download(const std::string & remote, const std::string & local,
        bool preserveTime, std::string & error)
    {
        return mScript.DownloadForEdit(remote, local, preserveTime, error);
    }

    virtual int Upload(const std::string & local, const std::string & remote,
        bool preserveTime, std::string & error)
    {
        return mScript.UploadForEdit(local, remote, preserveTime, error);
    }

private:
    Script & mScript;
};

} // namespace

bool Script::EditRemoteFile(const std::vector<std::string> & args)
{
    Console & console = Console::Instance();
    if (!RequireConnection())
    {
        return false;
    }

    EditOptions options;
    options.editor = mEditor;
    options.maxSize = mEditMaxSize;
    std::vector<std::string> paths;
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string & a = args[i];
        const std::string lower = ToLower(a);
        if (lower == "-force")
        {
            options.force = true;
        }
        else if (lower == "-keep" || lower == "-keeptemp")
        {
            options.keepTempFile = true;
        }
        else if (lower == "-binary")
        {
            options.allowBinary = true;
        }
        else if (StartsWith(lower, "-editor="))
        {
            options.editor = a.substr(strlen("-editor="));
        }
        else if (!a.empty() && a[0] == '-')
        {
            console.Error(Texts::WrongArguments("edit",
                "Unsupported switch '" + a + "'; it will not be ignored."));
            mErrors++;
            return false;
        }
        else
        {
            paths.push_back(a);
        }
    }

    if (paths.size() != 1)
    {
        console.Error(Texts::WrongArguments("edit",
            "Syntax: edit [-force] [-keep] [-editor=COMMAND] <remotefile>. "
            "One file at a time."));
        mErrors++;
        return false;
    }

    ScriptFileAccess access(*this);
    RemoteEditor editor(access, &console);
    std::string error;
    if (editor.Edit(ResolveRemotePath(mRemoteDirectory, paths[0]), options,
        mConfirm, error))
    {
        return true;
    }
    console.Error(error);
    mErrors++;
    return false;
}

/* ------------------------------------------------------------------ *
 * Stored sessions (the site manager)                                  *
 * ------------------------------------------------------------------ */

bool Script::SessionCommand(const std::vector<std::string> & args)
{
    Console & console = Console::Instance();
    const std::string path = mSessions.Path();

    if (args.size() >= 2 && ToLower(args[0]) == "export")
    {
        std::string error;
        if (args.size() != 2)
        {
            console.Error(Texts::WrongArguments("session",
                "Syntax: session export <file>."));
            mErrors++;
            return false;
        }
        if (!mSessions.ExportFile(ExpandTilde(args[1]), error))
        {
            console.Error(error);
            mErrors++;
            return false;
        }
        console.Status("Exported " + std::to_string(mSessions.Sessions().size()) +
            " session(s) to '" + args[1] + "'.");
        return true;
    }

    if (args.size() >= 2 && ToLower(args[0]) == "import")
    {
        if (args.size() != 2)
        {
            console.Error(Texts::WrongArguments("session",
                "Syntax: session import <file>."));
            mErrors++;
            return false;
        }
        const bool merge = true;
        std::string error;
        const int imported = mSessions.ImportFile(ExpandTilde(args[1]), merge, error);
        if (imported == 0)
        {
            console.Error("No sessions were imported from '" + args[1] + "'; " +
                (error.empty() ? std::string("the file has none.") : error));
            mErrors++;
            return false;
        }
        if (!mSessions.Save(error))
        {
            console.Error(error);
            mErrors++;
            return false;
        }
        console.Status("Imported " + std::to_string(imported) +
            " session(s) from '" + args[1] + "'.");
        return true;
    }

    if (args.size() == 1 && ToLower(args[0]) == "list")
    {
        return SessionCommand(std::vector<std::string>());
    }

    if (args.size() == 1)
    {
        const StoredSession * stored = mSessions.Find(args[0]);
        if (stored == NULL)
        {
            console.Error("There is no stored session named '" + args[0] + "'.");
            mErrors++;
            return false;
        }
        return OpenStoredSite(stored->name);
    }

    if (args.size() > 1)
    {
        console.Error(Texts::WrongArguments("session",
            "Syntax: session [NAME|list|export <file>|import <file>]."));
        mErrors++;
        return false;
    }

    if (mSessions.Empty())
    {
        console.WriteLine("There are no stored sessions. Use "
            "'savesession <name>' to store the current connection.");
        if (!path.empty())
        {
            console.WriteLine("Sessions are stored in " + path);
        }
        return true;
    }

    console.WriteLine("Stored sessions in " + (path.empty() ?
        std::string("(no configuration directory)") : path) + ":");
    const std::vector<StoredSession> & sessions = mSessions.Sessions();
    size_t width = 0;
    for (size_t i = 0; i < sessions.size(); i++)
    {
        width = sessions[i].name.size() > width ? sessions[i].name.size() : width;
    }
    for (size_t i = 0; i < sessions.size(); i++)
    {
        std::string url = sessions[i].protocol.empty() ?
            std::string("sftp") : sessions[i].protocol;
        url += "://";
        if (!sessions[i].userName.empty())
        {
            url += sessions[i].userName + "@";
        }
        url += sessions[i].hostName.empty() ? std::string("?") : sessions[i].hostName;
        if (sessions[i].portNumber > 0 && sessions[i].portNumber != 22)
        {
            url += ":" + std::to_string(sessions[i].portNumber);
        }
        std::string line = "  " + sessions[i].name;
        line.append(width - sessions[i].name.size() + 2, ' ');
        line += url;
        if (!sessions[i].remoteDirectory.empty())
        {
            line += "  -> " + sessions[i].remoteDirectory;
        }
        console.WriteLine(line);
    }
    return true;
}

bool Script::OpenStoredSite(const std::string & name)
{
    const StoredSession * stored = mSessions.Find(name);
    if (stored == NULL)
    {
        return false;
    }
    std::string error;
    if (mSession.IsOpen())
    {
        CloseSite();
    }

    SessionOptions options = SessionStore::ToSessionOptions(*stored, mDefaults);
    if (options.userName.empty())
    {
        options.userName = CurrentUserName();
    }
    Console & console = Console::Instance();
    console.Status("Connecting to " + options.hostName + " (stored session '" +
        stored->name + "')...");
    if (!mSession.Open(options, error))
    {
        console.Error(Texts::CannotOpenSession(options.hostName, error));
        mErrors++;
        return false;
    }

    if (UseNativeSftp(mSession.Options().protocol))
    {
        if (lscp_sftp_init() != 0)
        {
            console.Error(lscp_sftp_error());
            mSession.Close();
            mErrors++;
            return false;
        }
        std::string initial = options.initialDirectory.empty() ?
            std::string(".") : options.initialDirectory;
        char * canonical = lscp_sftp_realpath(initial.c_str());
        if (canonical == NULL)
        {
            console.Error(lscp_sftp_error());
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

    if (!stored->localDirectory.empty())
    {
        std::string localError;
        if (!SetLocalDirectory(ExpandTilde(stored->localDirectory), localError))
        {
            console.Error(Texts::LocalError("change directory",
                stored->localDirectory, localError));
        }
    }

    mStoredSessionName = stored->name;
    console.Status("Connected, the remote directory is " + mRemoteDirectory);

    /* Remember when this session was last used; a failure to record it
     * does not break the connection. */
    StoredSession updated = *stored;
    updated.lastUsed = static_cast<unsigned long long>(std::time(NULL));
    std::string saveError;
    if (mSessions.AddOrUpdate(updated, saveError))
    {
        mSessions.Save(saveError);
    }
    return true;
}

bool Script::SaveStoredSession(const std::vector<std::string> & args)
{
    Console & console = Console::Instance();
    if (args.empty())
    {
        console.Error(Texts::WrongArguments("savesession",
            "Syntax: savesession <name> [-host=HOST] [-port=N] [-username=NAME] "
            "[-protocol=SCHEME] [-privatekey=FILE] [-hostkey=KEY] "
            "[-remotedir=DIR] [-localdir=DIR] [-password] [-password=PWD]."));
        mErrors++;
        return false;
    }

    const std::string name = args[0];
    if (!SessionStore::IsValidName(name))
    {
        console.Error(Texts::WrongArguments("savesession",
            "The session name '" + name + "' is not valid."));
        mErrors++;
        return false;
    }

    SessionOptions options = mDefaults;
    bool connected = mSession.IsOpen();
    if (connected)
    {
        options = mSession.Options();
    }
    else
    {
        options.hostName.clear();
        options.initialDirectory.clear();
    }

    std::string localDirectory;
    std::string remoteDirectory = mRemoteDirectory;
    bool savePassword = false;
    std::string explicitPassword;
    bool passwordGiven = false;

    for (size_t i = 1; i < args.size(); i++)
    {
        const std::string & a = args[i];
        const std::string lower = ToLower(a);
        if (StartsWith(lower, "-host="))
        {
            options.hostName = a.substr(strlen("-host="));
        }
        else if (StartsWith(lower, "-port="))
        {
            options.portNumber = atoi(a.substr(strlen("-port=")).c_str());
        }
        else if (StartsWith(lower, "-username="))
        {
            options.userName = a.substr(strlen("-username="));
        }
        else if (StartsWith(lower, "-protocol="))
        {
            options.protocol = ToLower(a.substr(strlen("-protocol=")));
        }
        else if (StartsWith(lower, "-privatekey="))
        {
            options.privateKeyFile = ExpandTilde(a.substr(strlen("-privatekey=")));
        }
        else if (StartsWith(lower, "-hostkey="))
        {
            options.hostKey = a.substr(strlen("-hostkey="));
        }
        else if (StartsWith(lower, "-remotedir="))
        {
            remoteDirectory = a.substr(strlen("-remotedir="));
        }
        else if (StartsWith(lower, "-localdir="))
        {
            localDirectory = ExpandTilde(a.substr(strlen("-localdir=")));
        }
        else if (lower == "-password")
        {
            savePassword = true;
        }
        else if (StartsWith(lower, "-password="))
        {
            explicitPassword = a.substr(strlen("-password="));
            passwordGiven = true;
            savePassword = true;
        }
        else
        {
            console.Error(Texts::WrongArguments("savesession",
                "Unknown switch '" + a + "'."));
            mErrors++;
            return false;
        }
    }

    if (localDirectory.empty() && connected)
    {
        std::string error;
        localDirectory = GetLocalDirectory(error);
    }

    if (options.hostName.empty())
    {
        console.Error(Texts::WrongArguments("savesession",
            "There is no host to store: connect first, or give -host=HOST."));
        mErrors++;
        return false;
    }

    StoredSession session = SessionStore::FromSessionOptions(name, options,
        remoteDirectory, localDirectory);
    if (savePassword)
    {
        session.password = passwordGiven ? explicitPassword :
            (connected ? options.password : std::string());
        if (session.password.empty())
        {
            console.Error(Texts::WrongArguments("savesession",
                "The password of this connection is not known (it was typed or "
                "taken from a key or an agent), so it cannot be stored."));
            mErrors++;
            return false;
        }
    }

    std::string error;
    if (!mSessions.AddOrUpdate(session, error))
    {
        console.Error(error);
        mErrors++;
        return false;
    }
    if (!mSessions.Save(error))
    {
        console.Error(error);
        mErrors++;
        return false;
    }
    console.Status("Stored session '" + name + "' for " + session.protocol + "://" +
        session.userName + "@" + session.hostName +
        (mSessions.Path().empty() ? std::string() :
            std::string(" in ") + mSessions.Path()) + ".");
    if (!session.password.empty())
    {
        console.Status("Warning: the password was written to the session file. "
            "It is readable by this account only; remove it with "
            "'savesession " + name + "' after connecting with a key or an agent.");
    }
    return true;
}

bool Script::RemoveStoredSessionEntry(const std::vector<std::string> & args)
{
    Console & console = Console::Instance();
    if (args.size() != 1)
    {
        console.Error(Texts::WrongArguments("rmsession",
            "Syntax: rmsession <name>."));
        mErrors++;
        return false;
    }
    std::string error;
    if (!mSessions.Remove(args[0], error))
    {
        console.Error(error);
        mErrors++;
        return false;
    }
    if (!mSessions.Save(error))
    {
        console.Error(error);
        mErrors++;
        return false;
    }
    console.Status("Removed the stored session '" + args[0] + "'.");
    return true;
}

bool Script::RemoveRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    std::vector<std::string> files;
    bool onlyFile = false;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (args[i] == "-onlyfile")
        {
            onlyFile = true;
            continue;
        }
        files.push_back(args[i]);
    }
    if (files.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("rm",
            "Syntax: rm [-onlyfile] <file(s)>"));
        mErrors++;
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < files.size(); i++)
    {
        std::vector<std::string> paths;
        std::string error;
        if (!ExpandRemote(files[i], paths, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", files[i], error));
            mErrors++;
            ok = false;
            continue;
        }
        if (paths.empty())
        {
            if (mFailOnNoMatch)
            {
                Console::Instance().Error(Texts::RemoteError("find", files[i],
                    "No matching files."));
                mErrors++;
                ok = false;
            }
            else
            {
                Console::Instance().Status("No file matching '" + files[i] +
                    "' found.");
            }
            continue;
        }
        for (size_t n = 0; n < paths.size(); n++)
        {
            if (onlyFile)
            {
                LscpAttrs attrs = {};
                if (RemoteStat(paths[n], &attrs, error) == LSCP_SFTP_STAT_EXISTS &&
                    (attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
                    (attrs.permissions & 0040000) != 0)
                {
                    Console::Instance().Status("Skipped the directory '" +
                        paths[n] + "' (-onlyfile).");
                    continue;
                }
            }
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
    if (args.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("rmdir",
            "Syntax: rmdir <directory> [<directory2> ...]"));
        mErrors++;
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < args.size(); i++)
    {
        std::string path = ResolveRemotePath(mRemoteDirectory, args[i]);
        if (mConfirm)
        {
            ConfirmationResult confirmation = ConfirmDestructiveAction(
                "Delete remote directory '" + path + "'?");
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
        std::string error;
        if (RemoteRemoveDir(path, error) != 0)
        {
            Console::Instance().Error(Texts::RemoteError("remove the directory",
                path, error));
            mErrors++;
            ok = false;
        }
    }
    return ok;
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
    if (args.size() < 2)
    {
        Console::Instance().Error(Texts::WrongArguments("mv",
            "Syntax: mv <source> [ <source>... ] <target>"));
        mErrors++;
        return false;
    }
    std::string error;
    const std::string to = ResolveRemotePath(mRemoteDirectory, args.back());
    std::vector<std::string> sources(args.begin(), args.end() - 1);
    /* With several sources the target must be an existing directory,
     * exactly like upstream 'mv'. */
    if (sources.size() > 1)
    {
        LscpAttrs targetAttrs = {};
        const int targetStat = RemoteStat(to, &targetAttrs, error);
        const bool targetIsDir = targetStat == LSCP_SFTP_STAT_EXISTS &&
            (targetAttrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
            (targetAttrs.permissions & 0040000) != 0;
        if (!targetIsDir)
        {
            Console::Instance().Error(Texts::WrongArguments("mv",
                "When several files are moved at once, the target must be an "
                "existing directory: '" + to + "' is not."));
            mErrors++;
            return false;
        }
    }
    bool ok = true;
    for (size_t i = 0; i < sources.size(); i++)
    {
        std::string from = ResolveRemotePath(mRemoteDirectory, sources[i]);
        /* Moving into a directory target keeps the source name, unless
         * the target names the file itself. */
        std::string target = to;
        if (sources.size() > 1)
        {
            target = JoinRemotePath(to, RemoteFileName(from));
        }
        else
        {
            LscpAttrs targetAttrs = {};
            const int targetStat = RemoteStat(target, &targetAttrs, error);
            if (targetStat == LSCP_SFTP_STAT_EXISTS &&
                (targetAttrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
                (targetAttrs.permissions & 0040000) != 0)
            {
                target = JoinRemotePath(target, RemoteFileName(from));
            }
        }
        if (mConfirm)
        {
            LscpAttrs existing = {};
            const int targetStat = RemoteStat(target, &existing, error);
            if (targetStat == LSCP_SFTP_STAT_ERROR)
            {
                Console::Instance().Error(Texts::RemoteError("inspect target",
                    target, error));
                mErrors++;
                ok = false;
                continue;
            }
            if (targetStat == LSCP_SFTP_STAT_EXISTS)
            {
                ConfirmationResult confirmation = ConfirmDestructiveAction(
                    "Overwrite remote file '" + target + "'?");
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
        }
        if (RemoteRename(from, target, error) != 0)
        {
            Console::Instance().Error(Texts::RemoteError("move", from, error));
            mErrors++;
            ok = false;
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ *
 * Remote copy (cp), symbolic links (ln), checksums, exec and copyid    *
 * ------------------------------------------------------------------ */

namespace {

/* A scratch name for streamed copies and checksum fallbacks, made
 * unique with the process id and a counter. */
std::string MakeTempPath(const char * suffix)
{
    static unsigned long counter = 0;
    const std::string dir = TempDirectory();
    if (dir.empty())
    {
        return std::string();
    }
    for (unsigned int attempt = 0; attempt < 64; attempt++)
    {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "%s%slinuxscp-%ld-%s-%lu",
            dir.c_str(), kIsWindows ? "\\" : "/",
            static_cast<long>(getpid()), suffix, ++counter);
        const std::string path(buffer);
        FILE * probe = fopen(path.c_str(), "wx");
        if (probe != NULL)
        {
            fclose(probe);
            remove(path.c_str());
            return path;
        }
    }
    return std::string();
}

/* Joins the arguments of a command back into one command line, keeping
 * what quoting survived the split. */
std::string JoinCommandWords(const std::vector<std::string> & args)
{
    std::string command;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (i > 0)
        {
            command += " ";
        }
        if (args[i].find(' ') != std::string::npos)
        {
            command += "\"" + args[i] + "\"";
        }
        else
        {
            command += args[i];
        }
    }
    return command;
}

/* The remote checksum algorithm names the server side understands. */
std::string ServerChecksumAlgorithm(const std::string & requested)
{
    std::string lowered;
    for (size_t i = 0; i < requested.size(); i++)
    {
        const char c = static_cast<char>(tolower(
            static_cast<unsigned char>(requested[i])));
        if (c != '-' && c != '_')
        {
            lowered += c;
        }
    }
    if (lowered == "sha256") return "sha-256";
    if (lowered == "sha1") return "sha-1";
    if (lowered == "md5") return "md5";
    if (lowered == "crc32") return "crc32";
    return std::string();
}

} // namespace

int Script::RemoteSymlink(const std::string & target,
    const std::string & linkPath, std::string & error)
{
    error.clear();
    if (!UseNativeSftp(mSession.Options().protocol))
    {
        error = "Creating symbolic links needs the native SFTP backend; "
            "the '" + mSession.Options().protocol + "' protocol cannot do it.";
        return -1;
    }
    if (lscp_sftp_symlink(target.c_str(), linkPath.c_str()) != 0)
    {
        error = lscp_sftp_error();
        return -1;
    }
    return 0;
}

int Script::RemoteStreamCopy(const std::string & from, const std::string & to,
    std::string & error)
{
    /* Copy through this client: download to a scratch file, upload it,
     * remove it. Works with every backend, including curl. */
    const std::string temp = MakeTempPath("copy");
    if (temp.empty())
    {
        error = "Cannot create a temporary file for the copy.";
        return -1;
    }
    unsigned long long size = 0;
    if (RemoteGet(from, temp, 0, false, &size, NULL, NULL, error) != 0)
    {
        remove(temp.c_str());
        return -1;
    }
    const int result = RemotePut(temp, to, -1, 0, false, &size, NULL, NULL, error);
    remove(temp.c_str());
    return result;
}

bool Script::CopyRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() < 2)
    {
        Console::Instance().Error(Texts::WrongArguments("cp",
            "Syntax: cp <source> [ <source>... ] <target>"));
        mErrors++;
        return false;
    }

    /* The last argument is the target; everything before it is copied. */
    std::vector<std::string> sourceArgs(args.begin(), args.end() - 1);
    const std::string targetArg = args.back();
    const std::string target = ResolveRemotePath(mRemoteDirectory, targetArg);

    LscpAttrs targetAttrs = {};
    std::string error;
    const int targetStat = RemoteStat(target, &targetAttrs, error);
    if (targetStat == LSCP_SFTP_STAT_ERROR)
    {
        Console::Instance().Error(Texts::RemoteError("inspect target", target,
            error));
        mErrors++;
        return false;
    }
    const bool targetExists = targetStat == LSCP_SFTP_STAT_EXISTS;
    const bool targetIsDir = targetExists &&
        (targetAttrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
        (targetAttrs.permissions & 0040000) != 0;

    /* Expand every source mask into paths. */
    std::vector<std::string> sources;
    bool expansionFailed = false;
    for (size_t i = 0; i < sourceArgs.size(); i++)
    {
        std::vector<std::string> expanded;
        if (!ExpandRemote(sourceArgs[i], expanded, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", sourceArgs[i],
                error));
            mErrors++;
            expansionFailed = true;
            continue;
        }
        if (expanded.empty())
        {
            Console::Instance().Error(Texts::RemoteError("find", sourceArgs[i],
                "No matching files."));
            mErrors++;
            expansionFailed = true;
            continue;
        }
        for (size_t n = 0; n < expanded.size(); n++)
        {
            sources.push_back(expanded[n]);
        }
    }
    if (expansionFailed || sources.empty())
    {
        return false;
    }
    if (sources.size() > 1 && targetExists && !targetIsDir)
    {
        Console::Instance().Error(Texts::RemoteError("copy to", target,
            "A directory is required when copying multiple files."));
        mErrors++;
        return false;
    }

    bool any = false;
    bool ok = true;
    for (size_t i = 0; i < sources.size(); i++)
    {
        LscpAttrs sourceAttrs = {};
        const int sourceStat = RemoteStat(sources[i], &sourceAttrs, error);
        const bool sourceIsDir = sourceStat == LSCP_SFTP_STAT_EXISTS &&
            (sourceAttrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
            (sourceAttrs.permissions & 0040000) != 0;

        const std::string destination = (targetIsDir || sources.size() > 1) ?
            JoinRemotePath(target, RemoteFileName(sources[i])) : target;

        if (sourceIsDir)
        {
            if (!CopyRemoteTree(sources[i], destination))
            {
                ok = false;
                continue;
            }
            any = true;
            continue;
        }

        if (!CopyRemoteTree(sources[i], destination))
        {
            ok = false;
            continue;
        }
        any = true;
    }
    return any && ok;
}

/* Copies one file or a whole directory tree to one exact destination
 * path; the multi-source handling lives in CopyRemote. */
bool Script::CopyRemoteTree(const std::string & from, const std::string & to)
{
    std::string error;
    LscpAttrs attrs = {};
    const int statRes = RemoteStat(from, &attrs, error);
    const bool isDir = statRes == LSCP_SFTP_STAT_EXISTS &&
        (attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
        (attrs.permissions & 0040000) != 0;

    if (!isDir)
    {
        return CopyRemoteOneFile(from, to);
    }

    if (!CreateRemoteDirectoryRecursive(to, error))
    {
        Console::Instance().Error(Texts::RemoteError("create the directory",
            to, error));
        mErrors++;
        return false;
    }
    std::vector<SyncEntry> entries;
    if (!ListRemoteEntries(from, entries, error))
    {
        Console::Instance().Error(Texts::RemoteError("read directory", from,
            error));
        mErrors++;
        return false;
    }
    bool ok = true;
    for (size_t n = 0; n < entries.size(); n++)
    {
        if (!CopyRemoteTree(JoinRemotePath(from, entries[n].name),
                JoinRemotePath(to, entries[n].name)))
        {
            ok = false;
        }
    }
    return ok;
}

/* Copies one file to one exact destination path. */
bool Script::CopyRemoteOneFile(const std::string & from,
    const std::string & to)
{
    std::string error;
    if (mConfirm)
    {
        LscpAttrs existing = {};
        const int statRes = RemoteStat(to, &existing, error);
        if (statRes == LSCP_SFTP_STAT_ERROR)
        {
            Console::Instance().Error(Texts::RemoteError("inspect target", to,
                error));
            mErrors++;
            return false;
        }
        if (statRes == LSCP_SFTP_STAT_EXISTS)
        {
            const ConfirmationResult confirmation = ConfirmDestructiveAction(
                "Overwrite remote file '" + to + "'?");
            if (confirmation != ConfirmationResult::Accepted)
            {
                return confirmation != ConfirmationResult::Unavailable;
            }
        }
    }

    if (UseNativeSftp(mSession.Options().protocol))
    {
        const int result = lscp_sftp_copy(from.c_str(), to.c_str());
        if (result == 0)
        {
            Console::Instance().Status("Copied '" + from + "' to '" + to +
                "' on the server.");
            return true;
        }
        if (result == -1)
        {
            Console::Instance().Error(Texts::RemoteError("copy", from,
                lscp_sftp_error()));
            mErrors++;
            return false;
        }
        Console::Instance().Status("The server cannot copy files by itself; " +
            std::string("copying through this client."));
    }

    if (RemoteStreamCopy(from, to, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("copy", from, error));
        mErrors++;
        return false;
    }
    Console::Instance().Status("Copied '" + from + "' to '" + to + "'.");
    return true;
}

bool Script::LinkRemote(const std::vector<std::string> & args, const char * name)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments(name,
            "Syntax: " + std::string(name) + " <target> <link>"));
        mErrors++;
        return false;
    }
    const std::string target = ResolveRemotePath(mRemoteDirectory, args[0]);
    const std::string linkPath = ResolveRemotePath(mRemoteDirectory, args[1]);
    std::string error;
    if (RemoteSymlink(target, linkPath, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("create the symbolic link",
            linkPath, error));
        mErrors++;
        return false;
    }
    Console::Instance().Status("Created '" + linkPath + "' pointing at '" +
        target + "'.");
    return true;
}

bool Script::ChecksumRemote(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments("checksum",
            "Syntax: checksum <algorithm> <file> (algorithm: sha-256, "
            "sha-1, md5 or crc32)"));
        mErrors++;
        return false;
    }
    const std::string requested = args[0];
    const std::string path = ResolveRemotePath(mRemoteDirectory, args[1]);

    /* A directory has no checksum of its own. */
    LscpAttrs attrs = {};
    std::string error;
    if (RemoteStat(path, &attrs, error) != LSCP_SFTP_STAT_EXISTS)
    {
        Console::Instance().Error(Texts::RemoteError("read the attributes of",
            path, error));
        mErrors++;
        return false;
    }
    if ((attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
        (attrs.permissions & 0040000) != 0)
    {
        Console::Instance().Error(Texts::WrongArguments("checksum",
            "'" + path + "' is a directory, not a file."));
        mErrors++;
        return false;
    }

    /* 1. Ask the server, when the backend can. */
    if (UseNativeSftp(mSession.Options().protocol))
    {
        const std::string serverAlgorithm = ServerChecksumAlgorithm(requested);
        if (!serverAlgorithm.empty())
        {
            char used[64];
            char hex[LSCP_HASH_HEX_SIZE];
            const int result = lscp_sftp_checksum(serverAlgorithm.c_str(),
                path.c_str(), used, sizeof(used), hex, sizeof(hex));
            if (result == 0)
            {
                Console::Instance().WriteLine(std::string(hex) + " " + path);
                return true;
            }
            if (result == -1)
            {
                Console::Instance().Error(Texts::RemoteError(
                    "calculate the checksum of", path, lscp_sftp_error()));
                mErrors++;
                return false;
            }
            Console::Instance().Status("The server cannot calculate "
                "checksums; '" + requested + "' is computed from the "
                "downloaded file.");
        }
        else
        {
            Console::Instance().Status("'" + requested + "' is not a server-"
                "side algorithm; it is computed from the downloaded file.");
        }
    }
    else
    {
        Console::Instance().Status("The '" + mSession.Options().protocol +
            "' backend cannot run a server-side checksum; '" + requested +
            "' is computed from the downloaded file.");
    }

    /* 2. Fallback: hash the file as it is downloaded. */
    const std::string temp = MakeTempPath("sum");
    if (temp.empty())
    {
        Console::Instance().Error("Cannot create a temporary file for the "
            "checksum.");
        mErrors++;
        return false;
    }
    unsigned long long size = 0;
    if (RemoteGet(path, temp, 0, false, &size, NULL, NULL, error) != 0)
    {
        remove(temp.c_str());
        Console::Instance().Error(Texts::TransferFailed(path, error));
        mErrors++;
        return false;
    }
    char hex[LSCP_HASH_HEX_SIZE];
    const bool hashed = lscp_hash_file(requested.c_str(), temp.c_str(),
        hex, sizeof(hex)) == 0;
    remove(temp.c_str());
    if (!hashed)
    {
        Console::Instance().Error(Texts::RemoteError("calculate the checksum of",
            path, lscp_hash_error()));
        mErrors++;
        return false;
    }
    Console::Instance().WriteLine(std::string(hex) + " " + path);
    return true;
}

bool Script::CallRemote(const std::vector<std::string> & args, const char * name)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.empty())
    {
        Console::Instance().Error(Texts::WrongArguments(name,
            "Syntax: " + std::string(name) + " <command>"));
        mErrors++;
        return false;
    }
    const SessionOptions & options = mSession.Options();
    if (!UseNativeSftp(options.protocol))
    {
        Console::Instance().Error(Texts::WrongArguments(name,
            "Executing a command on the server needs an SSH (sftp://) "
            "session, not '" + options.protocol + "'."));
        mErrors++;
        return false;
    }
    const std::string command = JoinCommandWords(args);
    std::string error;
    if (!Session::ExecRemote(options, command, error))
    {
        Console::Instance().Error(Texts::RemoteError("run the command on",
            options.hostName, error.empty() ? "the connection failed" : error));
        mErrors++;
        return false;
    }
    return true;
}

#if !defined(_WIN32)
namespace {

/* The part of an authorized_keys line that says which key it is: the
 * algorithm and the base64 blob, without the trailing comment. Two lines
 * with the same identity are the same key, however differently they are
 * commented. */
std::string KeyIdentity(const std::string & line)
{
    size_t end = 0;
    for (int field = 0; field < 2; field++)
    {
        while (end < line.size() && line[end] == ' ')
        {
            end++;
        }
        while (end < line.size() && line[end] != ' ')
        {
            end++;
        }
    }
    return line.substr(0, end);
}

} // namespace
#endif

/*
 * 'copyid [<privatekey>]': installs the public half of a key on the server,
 * which is upstream's 'copyid'. The key is read with the bundled PuTTY core,
 * so a PuTTY .ppk, an OpenSSH public key and an OpenSSH private key all work;
 * the line is appended to ~/.ssh/authorized_keys unless that file already
 * holds the key. The permissions matter to sshd, which ignores a key under a
 * group-writable directory, so both are set as the server expects.
 */
bool Script::CopyPublicKey(const std::vector<std::string> & args)
{
    if (args.size() > 1)
    {
        return SwitchError("copyid", "Syntax: copyid [<keyfile>]; without a "
            "file the private key of the session is used.");
    }
#if defined(_WIN32)
    (void)args;
    /* Say so before connecting, rather than after the user has typed a
     * passphrase. */
    return SwitchError("copyid", "Reading a key file needs the POSIX build, "
        "where the bundled PuTTY core can decode one.");
#else
    if (!RequireConnection())
    {
        return false;
    }
    const SessionOptions & options = mSession.Options();
    if (options.protocol != "sftp" && options.protocol != "scp")
    {
        return SwitchError("copyid", "Installing a public key needs an SSH "
            "(sftp:// or scp://) session, not '" + options.protocol + "'.");
    }
    std::string keyFile = args.empty() ? options.privateKeyFile : args[0];
    if (keyFile.empty())
    {
        return SwitchError("copyid", "There is no private key: connect with "
            "-privatekey=<file>, or give the key file to the command.");
    }
    std::string keyLine;
    std::string error;
    if (!PublicKeyAuthorizedLine(ExpandTilde(keyFile), keyLine, error))
    {
        return SwitchError("copyid", error);
    }
    if (mHomeDirectory.empty())
    {
        return SwitchError("copyid", "The home directory of the session is "
            "not known, so ~/.ssh/authorized_keys cannot be located.");
    }

    const std::string sshDirectory = JoinRemotePath(mHomeDirectory, ".ssh");
    const std::string authorizedKeys = JoinRemotePath(sshDirectory, "authorized_keys");

    /* Create the directory when the server has none yet. */
    LscpAttrs attrs = {};
    std::string statError;
    if (RemoteStat(sshDirectory, &attrs, statError) != LSCP_SFTP_STAT_EXISTS)
    {
        std::string makeError;
        if (RemoteMakeDir(sshDirectory, 0700, makeError) != 0)
        {
            mErrors++;
            return SwitchError("copyid", "Cannot create '" + sshDirectory +
                "': " + makeError);
        }
        Console::Instance().Status("Created '" + sshDirectory + "'.");
    }

    /* Keep the keys that are installed already; a repeated 'copyid' must not
     * grow the file, and a server with a full authorized_keys is a common
     * leftover of a previous WinSCP run. */
    const std::string temp = MakeTempPath("authkeys");
    if (temp.empty())
    {
        return SwitchError("copyid", "Cannot create a temporary file for the "
            "authorized keys.");
    }
    bool installed = false;
    bool failed = false;
    std::vector<std::string> kept;
    statError.clear();
    if (RemoteStat(authorizedKeys, &attrs, statError) == LSCP_SFTP_STAT_EXISTS)
    {
        std::string getError;
        if (RemoteGet(authorizedKeys, temp, 0, false, NULL, NULL, NULL, getError) != 0)
        {
            remove(temp.c_str());
            mErrors++;
            return SwitchError("copyid", "Cannot read '" + authorizedKeys +
                "': " + getError);
        }
        FILE * file = fopen(temp.c_str(), "rb");
        if (file != NULL)
        {
            char buffer[16384];
            while (fgets(buffer, sizeof(buffer), file) != NULL)
            {
                std::string line = buffer;
                while (!line.empty() && (line[line.size() - 1] == '\n' ||
                    line[line.size() - 1] == '\r'))
                {
                    line.erase(line.size() - 1);
                }
                if (line.empty())
                {
                    continue;
                }
                kept.push_back(line);
                if (KeyIdentity(line) == KeyIdentity(keyLine))
                {
                    installed = true;
                }
            }
            fclose(file);
        }
    }

    if (installed)
    {
        remove(temp.c_str());
        Console::Instance().Status("The key is already installed in '" +
            authorizedKeys + "'.");
        return true;
    }

    FILE * file = fopen(temp.c_str(), "wb");
    if (file == NULL)
    {
        return SwitchError("copyid", "Cannot write a temporary file for the "
            "authorized keys.");
    }
    for (size_t i = 0; i < kept.size(); i++)
    {
        fprintf(file, "%s\n", kept[i].c_str());
    }
    fprintf(file, "%s\n", keyLine.c_str());
    fclose(file);

    std::string putError;
    if (RemotePut(temp, authorizedKeys, 0600, 0, false, NULL, NULL, NULL,
        putError) != 0)
    {
        failed = true;
        Console::Instance().Error(Texts::TransferFailed(authorizedKeys, putError));
    }
    remove(temp.c_str());
    if (failed)
    {
        mErrors++;
        return false;
    }
    Console::Instance().Status("Installed the public key of '" + keyFile +
        "' in '" + authorizedKeys + "'.");
    return true;
#endif
}

bool Script::ApplyRemotePermissions(const std::string & path, const std::string & mode,
    bool recursive, unsigned long long & changed)
{
    LscpAttrs attrs = {};
    std::string error;
    if (RemoteStat(path, &attrs, error) != LSCP_SFTP_STAT_EXISTS)
    {
        Console::Instance().Error(Texts::RemoteError("read the attributes of", path, error));
        mErrors++;
        return false;
    }
    const bool hasPermissions = (attrs.flags & LSCP_ATTR_PERMISSIONS) != 0;
    const bool isDirectory = hasPermissions && (attrs.permissions & 0040000) != 0;
    const unsigned long current = hasPermissions ? static_cast<unsigned long>(attrs.permissions & 07777) : 0;

    unsigned long wanted = 0;
    std::string modeError;
    if (!ParsePermissions(mode, current, isDirectory, wanted, modeError))
    {
        Console::Instance().Error("'" + path + "': " + modeError);
        mErrors++;
        return false;
    }
    /* Only the permission bits change; the file type and the set-id bits
     * that the mode does not mention stay as they are. */
    const unsigned long result = hasPermissions ?
        ((attrs.permissions & ~07777UL) | (wanted & 07777)) : (wanted & 07777);

    bool ok = true;
    if (RemoteSetPermissions(path, (int)result, error) != 0)
    {
        Console::Instance().Error(Texts::RemoteError("change the permissions of",
            path, error));
        mErrors++;
        ok = false;
    }
    else
    {
        changed++;
    }

    if (ok && recursive && isDirectory)
    {
        LscpNames * names = NULL;
        std::string dirError;
        if (!RemoteListDir(path, names, dirError))
        {
            Console::Instance().Error(Texts::RemoteError("read directory", path, dirError));
            mErrors++;
            return false;
        }
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
            const unsigned long type = names->names[i].attrs.permissions & 0170000;
            if (type == 0120000)
            {
                continue;   /* a symbolic link is left alone */
            }
            if (!ApplyRemotePermissions(JoinRemotePath(path, name), mode, true, changed))
            {
                ok = false;
            }
        }
        RemoteFreeNames(names);
    }
    return ok;
}

bool Script::ChangeRemotePermissions(const std::vector<std::string> & args)
{
    /* The mode is checked before the connection, so that a typo is reported
     * even when nothing can be changed, and no half-parsed command reaches
     * the server. */
    /* The switch may come before or after the mode. */
    bool recursive = false;
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); i++)
    {
        if (args[i] == "-recursive" || args[i] == "-r")
        {
            recursive = true;
        }
        else
        {
            rest.push_back(args[i]);
        }
    }
    if (rest.size() < 2)
    {
        Console::Instance().Error(Texts::WrongArguments("chmod",
            "Syntax: chmod <permissions> <file(s)> [-recursive]"));
        mErrors++;
        return false;
    }
    const std::string mode = rest[0];

    /* Check the mode before anything is touched, so that a typo cannot
     * change the first file and then fail on the second. */
    unsigned long probe = 0;
    std::string modeError;
    if (!ParsePermissions(mode, 0644, false, probe, modeError))
    {
        Console::Instance().Error(Texts::WrongArguments("chmod",
            "'" + mode + "' is not a valid mode: " + modeError +
            ". Use an octal number such as 644 or an expression such as u+x."));
        mErrors++;
        return false;
    }
    if (!RequireConnection())
    {
        return false;
    }

    bool ok = true;
    unsigned long long changed = 0;
    for (size_t i = 1; i < rest.size(); i++)
    {
        std::vector<std::string> paths;
        std::string error;
        if (!ExpandRemote(rest[i], paths, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", rest[i], error));
            mErrors++;
            ok = false;
            continue;
        }
        if (paths.empty())
        {
            Console::Instance().Error(Texts::RemoteError("find", rest[i],
                "No matching files."));
            mErrors++;
            ok = false;
            continue;
        }
        for (size_t n = 0; n < paths.size(); n++)
        {
            if (!ApplyRemotePermissions(paths[n], mode, recursive, changed))
            {
                ok = false;
            }
        }
    }
    if (recursive)
    {
        Console::Instance().WriteLine(std::to_string(changed) +
            (changed == 1 ? " file changed." : " files changed."));
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
    if ((attrs.flags & LSCP_ATTR_SIZE) != 0)
    {
        Console::Instance().WriteLine("SizeBytes:   " + std::to_string(attrs.size));
    }
    if ((attrs.flags & LSCP_ATTR_ACMODTIME) != 0)
    {
        /* Seconds since the epoch, for a script or the graphical client
         * that wants to compare timestamps rather than read them. */
        Console::Instance().WriteLine("ModifiedEpoch: " + std::to_string(attrs.mtime));
        Console::Instance().WriteLine("AccessedEpoch: " + std::to_string(attrs.atime));
    }
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
    const std::string supported =
        "supported: echo, batch, confirm, resume, transfer, include, exclude, "
        "failonnomatch, synchdelete, reconnecttime, editor, editmaxsize";
    if (args.empty())
    {
        /* 'option' with no name lists every option and its value, like
         * the upstream client. */
        const char * names[] = { "echo", "batch", "confirm", "resume",
            "transfer", "include", "exclude", "failonnomatch", "synchdelete",
            "reconnecttime", "editor", "editmaxsize" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        {
            std::vector<std::string> one;
            one.push_back(names[i]);
            SetOption(one);
        }
        return true;
    }
    std::string name = ToLower(args[0]);
    /*
     * Upstream looks an option name up in the same table as the commands,
     * so 'option synchdel on' and 'option e off' are as good as the full
     * names. An unknown name is left alone, so that the message at the end
     * of this function still says which option this build does not have.
     */
    if (name != "clear")
    {
        std::string resolved;
        if (!ResolveAbbreviation(OptionNames(), name, "option name", resolved))
        {
            return false;
        }
        if (!resolved.empty())
        {
            name = resolved;
        }
    }
    if (args.size() == 1)
    {
        if (name == "echo")
        {
            Console::Instance().Status(std::string("echo = ") +
                (mEchoCommands ? "on" : "off"));
            return true;
        }
        if (name == "confirm")
        {
            Console::Instance().Status(std::string("confirm = ") +
                (mConfirm ? "on" : "off"));
            return true;
        }
        if (name == "batch")
        {
            const char * value = mBatchMode == BatchMode::Abort ? "abort" :
                mBatchMode == BatchMode::Continue ? "continue" :
                mBatchMode == BatchMode::On ? "on" : "off";
            Console::Instance().Status(std::string("batch = ") + value);
            return true;
        }
        if (name == "resume")
        {
            Console::Instance().Status(std::string("resume = ") +
                (mResume ? "on" : "off"));
            return true;
        }
        if (name == "transfer")
        {
            Console::Instance().Status(std::string("transfer = ") +
                TransferModeName(mTransferMode));
            return true;
        }
        if (name == "include")
        {
            std::string masks;
            for (size_t i = 0; i < mOptionMask.include.size(); i++)
            {
                if (i != 0)
                {
                    masks += ";";
                }
                masks += mOptionMask.include[i].text;
            }
            Console::Instance().Status("include = " +
                (masks.empty() ? std::string("(none)") : masks));
            return true;
        }
        if (name == "exclude")
        {
            std::string masks;
            for (size_t i = 0; i < mOptionMask.exclude.size(); i++)
            {
                if (i != 0)
                {
                    masks += ";";
                }
                masks += mOptionMask.exclude[i].text;
            }
            Console::Instance().Status("exclude = " +
                (masks.empty() ? std::string("(none)") : masks));
            return true;
        }
        if (name == "failonnomatch")
        {
            Console::Instance().Status(std::string("failonnomatch = ") +
                (mFailOnNoMatch ? "on" : "off"));
            return true;
        }
        if (name == "synchdelete")
        {
            Console::Instance().Status(std::string("synchdelete = ") +
                (mSyncDeleteDefault ? "on" : "off"));
            return true;
        }
        if (name == "reconnecttime")
        {
            Console::Instance().Status(std::string("reconnecttime = ") +
                (mReconnectSeconds > 0 ? std::to_string(mReconnectSeconds) :
                std::string("off")));
            return true;
        }
        if (name == "editor")
        {
            Console::Instance().Status(std::string("editor = ") +
                (mEditor.empty() ? std::string("(from LINUXSCP_EDITOR, VISUAL or EDITOR)") : mEditor));
            return true;
        }
        if (name == "editmaxsize")
        {
            Console::Instance().Status("editmaxsize = " + std::to_string(mEditMaxSize));
            return true;
        }
    }
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments("option",
            "Syntax: option [<name> [<value>]] (" + supported + ")"));
        mErrors++;
        return false;
    }
    std::string value = ToLower(args[1]);
    /*
     * The value of an option that comes from a fixed vocabulary is resolved
     * the same way ('option batch cont'), but only for those options: a file
     * mask, an editor command and a size are free text and must never be
     * second-guessed.
     */
    if (name != "include" && name != "exclude" && name != "editor" &&
        name != "editmaxsize" && name != "reconnecttime")
    {
        std::vector<std::string> vocabulary = ToggleNames();
        if (name == "batch")
        {
            vocabulary = BatchModeNames();
        }
        else if (name == "transfer")
        {
            vocabulary = TransferModeNames();
        }
        std::string resolved;
        if (!ResolveAbbreviation(vocabulary, value,
            "value of the option '" + name + "'", resolved))
        {
            return false;
        }
        if (!resolved.empty())
        {
            value = resolved;
        }
    }
    const bool isOn = value == "on";
    const bool isToggle = isOn || value == "off";
    if ((name == "echo" || name == "failonnomatch" || name == "synchdelete") &&
        isToggle)
    {
        if (name == "echo")
        {
            mEchoCommands = isOn;
        }
        else if (name == "failonnomatch")
        {
            mFailOnNoMatch = isOn;
        }
        else
        {
            mSyncDeleteDefault = isOn;
        }
        return true;
    }
    if (name == "confirm" && isToggle)
    {
        mConfirm = isOn;
        return true;
    }
    if (name == "resume" && isToggle)
    {
        mResume = isOn;
        return true;
    }
    if (name == "transfer")
    {
        TransferMode mode;
        if (!ParseTransferMode(value, mode))
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'transfer' must be binary, ascii or automatic."));
            mErrors++;
            return false;
        }
        mTransferMode = mode;
        return true;
    }
    if (name == "include" || name == "exclude")
    {
        /* 'clear' resets the whole persistent mask. */
        if (value == "clear")
        {
            mOptionMask = FileMask();
            mOptionMaskUsed = false;
            return true;
        }
        FileMask parsed;
        std::string maskError;
        if (!parsed.Parse(args[1], maskError))
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                maskError));
            mErrors++;
            return false;
        }
        /* Setting one half replaces the whole mask, like upstream:
         * 'option exclude *.bak' forgets a previous 'option include'. */
        FileMask combined;
        if (name == "include")
        {
            combined.include = parsed.include;
        }
        else
        {
            combined.exclude = parsed.exclude;
        }
        mOptionMask = combined;
        mOptionMaskUsed = true;
        return true;
    }
    if (name == "reconnecttime")
    {
        if (value == "off")
        {
            mReconnectSeconds = 0;
            return true;
        }
        int seconds = 0;
        for (size_t i = 0; i < args[1].size(); i++)
        {
            if (args[1][i] < '0' || args[1][i] > '9')
            {
                seconds = -1;
                break;
            }
            seconds = seconds * 10 + (args[1][i] - '0');
            if (seconds > 3600)
            {
                break;
            }
        }
        if (seconds < 1 || seconds > 3600)
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'reconnecttime' must be a number of seconds "
                "from 1 to 3600, or 'off'."));
            mErrors++;
            return false;
        }
        mReconnectSeconds = seconds;
        return true;
    }
    if (name == "batch")
    {
        if (value == "on")
        {
            mBatchMode = BatchMode::On;
        }
        else if (value == "abort")
        {
            mBatchMode = BatchMode::Abort;
        }
        else if (value == "continue")
        {
            mBatchMode = BatchMode::Continue;
        }
        else if (value == "off")
        {
            mBatchMode = BatchMode::Off;
        }
        else
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'batch' must be 'on', 'off', 'abort' or 'continue'."));
            mErrors++;
            return false;
        }
        mBatchConfigured = true;
        Console::Instance().SetBatchMode(mBatchMode != BatchMode::Off);
        return true;
    }
    if (name == "editor")
    {
        mEditor = args[1];
        return true;
    }
    if (name == "editmaxsize")
    {
        unsigned long long bytes = 0;
        for (size_t i = 0; i < args[1].size(); i++)
        {
            if (args[1][i] < '0' || args[1][i] > '9')
            {
                Console::Instance().Error(Texts::WrongArguments("option",
                    "The value of 'editmaxsize' must be a number of bytes."));
                mErrors++;
                return false;
            }
            bytes = bytes * 10ULL + static_cast<unsigned long long>(args[1][i] - '0');
            if (bytes > (1ULL << 40))
            {
                break;
            }
        }
        if (bytes == 0)
        {
            Console::Instance().Error(Texts::WrongArguments("option",
                "The value of 'editmaxsize' must be a positive number of bytes."));
            mErrors++;
            return false;
        }
        mEditMaxSize = bytes;
        return true;
    }
    Console::Instance().Error(Texts::WrongArguments("option",
        "The option '" + args[0] + "' is not supported by this build."));
    mErrors++;
    return false;
}

/* Help text of one command. Upstream keeps these in its resource
 * strings; this is the equivalent for the implemented subset, close to
 * the wording of the WinSCP documentation. */
const char * Script::CommandHelp(const std::string & command)
{
    static const struct
    {
        const char * name;
        const char * help;
    } helps[] = {
        { "ascii", "ascii\n"
            "  Transfers files in ASCII mode: line endings are converted\n"
            "  (CRLF to LF when downloading, LF to CRLF when uploading)." },
        { "binary", "binary\n"
            "  Transfers files in binary mode (no conversion)." },
        { "bye", "bye\n"
            "  Ends the session and exits." },
        { "call", "call <command>\n"
            "  Executes a command on the server through the SSH 'exec'\n"
            "  channel and prints its output. '! <command>' is the same." },
        { "cd", "cd [ <directory> ]\n"
            "  Changes the remote working directory; without an argument\n"
            "  it goes to the home directory." },
        { "checksum", "checksum <algorithm> <file>\n"
            "  Calculates a checksum (sha-256, sha-1, md5 or crc32) of a\n"
            "  remote file, on the server when it can, locally otherwise." },
        { "chmod", "chmod <mode> <file> [ <file>... ] [ -recursive ]\n"
            "  Changes permissions of remote files. <mode> is octal\n"
            "  (644, 0700) or symbolic (u+x, go-w, a=r,go-w, u+s); with\n"
            "  -recursive everything below a directory is changed too,\n"
            "  symbolic links excepted." },
        { "close", "close\n"
            "  Closes the session." },
        { "copyid", "copyid [ <keyfile> ]\n"
            "  Installs the public key of the private key in use (or of\n"
            "  <keyfile>) into ~/.ssh/authorized_keys on the server, so\n"
            "  that later connections need no password. Requires a SFTP\n"
            "  or SCP session and a key this build can read." },
        { "cp", "cp <source> [ <source>... ] <target>\n"
            "  Copies files on the server, server-side when it can,\n"
            "  streamed through this client otherwise." },
        { "echo", "echo <message>\n"
            "  Prints the message." },
        { "edit", "edit <file>\n"
            "  Downloads the file, opens it in the configured editor and\n"
            "  uploads it back when it changed." },
        { "exit", "exit\n"
            "  Ends the session and exits." },
        { "get", "get [ <switches> ] <file> [ <file>... ] [ <target> ]\n"
            "  Downloads files. Switches: -delete, -resume, -append,\n"
            "  -resumesupport=on|off|<KB>, -speed=<KB/s>, -neweronly,\n"
            "  -noneweronly, -preservetime, -preservetime=all,\n"
            "  -nopreservetime, -r, -transfer=binary|ascii|automatic,\n"
            "  -filemask=<mask>." },
        { "help", "help [ <command> ]\n"
            "  Prints the list of commands, or the help of one command." },
        { "keepuptodate", "keepuptodate [ <localdir> [ <remotedir> ] ]\n"
            "  Repeats a synchronization on a timer; -interval=<seconds>,\n"
            "  -count=<passes>, -delete, -continue, -filemask=, -transfer=." },
        { "lcd", "lcd <directory>\n"
            "  Changes the local working directory." },
        { "lls", "lls [ <directory>[/<mask>] ]\n"
            "  Lists the local working directory." },
        { "ln", "ln <target> <link>\n"
            "  Creates a symbolic link on the server." },
        { "lpwd", "lpwd\n"
            "  Prints the local working directory." },
        { "ls", "ls [ <directory>[/<mask>] ]\n"
            "  Lists the remote directory (switches: -l, -a, -1)." },
        { "man", "man [ <command> ]\n"
            "  Same as 'help'." },
        { "mkdir", "mkdir <directory>\n"
            "  Creates a remote directory." },
        { "mv", "mv <source> [ <source>... ] <target>\n"
            "  Moves or renames remote files; with several sources the\n"
            "  target must be an existing directory." },
        { "open", "open <site>\n"
            "  Connects to a site given as a URL or a stored session name." },
        { "option", "option [ <name> [ <value> ] ]\n"
            "  Shows or changes a setting: echo, batch, confirm, resume,\n"
            "  transfer, include, exclude, failonnomatch, synchdelete,\n"
            "  reconnecttime, editor, editmaxsize. Without a name it\n"
            "  prints every setting; a name or a value may be abbreviated\n"
            "  as long as the abbreviation is unambiguous." },
        { "put", "put [ <switches> ] <file> [ <file>... ] [ <target> ]\n"
            "  Uploads files; the switches of 'get' plus -permissions=\n"
            "  and -nopermissions; -append adds to the remote file\n"
            "  instead of replacing it (native SFTP only, and not\n"
            "  together with -resume)." },
        { "pwd", "pwd\n"
            "  Prints the remote working directory." },
        { "rm", "rm [ -onlyfile ] <file> [ <file>... ]\n"
            "  Deletes remote files (masks allowed); -onlyfile skips\n"
            "  directories." },
        { "rmdir", "rmdir <directory> [ <directory>... ]\n"
            "  Deletes empty remote directories." },
        { "session", "session [ export <file> | import <file> ]\n"
            "  Lists stored sessions, or exports/imports them." },
        { "stat", "stat <file>\n"
            "  Prints the attributes of a remote file." },
        { "synchronize", "synchronize local|remote|both [ <localdir> [ <remotedir> ] ]\n"
            "  [ -delete ] [-nodelete] [-mirror] [-preview]\n"
            "  [-criteria=time|size|either|none] [-filemask=<mask>]\n"
            "  [-transfer=binary|ascii|automatic] [-nonrecursive]" },
        { "symlink", "symlink <target> <link>\n"
            "  Same as 'ln'." },
        { "terminal", "terminal\n"
            "  Opens an interactive shell terminal on the server." },
    };
    for (size_t i = 0; i < sizeof(helps) / sizeof(helps[0]); i++)
    {
        if (command == helps[i].name)
        {
            return helps[i].help;
        }
    }
    return NULL;
}

void Script::PrintHelp(const std::vector<std::string> & args)
{
    if (!args.empty())
    {
        for (size_t i = 0; i < args.size(); i++)
        {
            const char * help = CommandHelp(ToLower(args[i]));
            if (help == NULL)
            {
                Console::Instance().Error(Texts::UnknownCommand(args[i]));
                mErrors++;
                continue;
            }
            Console::Instance().WriteLine(help);
        }
        return;
    }
    Console::Instance().WriteLine(Texts::Usage());
}

bool Script::ExecuteLine(const std::string & line)
{
    if (mExit)
    {
        return false;
    }
    /* 'option echo on' prints every command as it runs, so that a log
     * file shows what a script did. The -server loop must stay silent. */
    if (mEchoCommands && !mServerLoop)
    {
        Console::Instance().WriteLine(CensoredForLog(line));
    }
    const int before = mErrors;
    /* The action log records what was asked and, afterwards, how it ended;
     * the messages in between belong to this command. */
    if (Console::Instance().IsActionLogOpen())
    {
        Console::Instance().LogAction("command",
            "line=\"" + Console::EscapeActionText(CensoredForLog(line)) + "\"");
    }
    const bool keepRunning = ExecuteCommand(line);
    mLastCommandFailed = mErrors != before;
    if (Console::Instance().IsActionLogOpen())
    {
        Console::Instance().LogAction("result",
            "status=\"" + std::string(mLastCommandFailed ? "error" : "ok") + "\"");
    }
    if (mErrors != before && (mBatchMode == BatchMode::Abort || mBatchMode == BatchMode::On))
    {
        mExit = true;
    }
    return keepRunning && !mExit;
}

bool Script::ExecuteCommand(const std::string & line)
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

    /*
     * Upstream keeps the commands in a table and runs the single command an
     * abbreviation can only mean, so 'synch local remote' works. An
     * abbreviation of several commands is refused instead of guessing; 'x'
     * would be 'exit' or nothing at all, and 'get' versus 'g' is a typo
     * waiting to move files.
     */
    if (!IsKnownCommand(command))
    {
        std::string resolved;
        if (ResolveAbbreviation(CommandNames(), command, "command", resolved))
        {
            if (!resolved.empty())
            {
                command = resolved;
            }
        }
        else
        {
            return true;
        }
    }

    if (command == "exit" || command == "bye" || command == "quit" || command == "end")
    {
        if (!args.empty())
        {
            Console::Instance().Error(Texts::WrongArguments(command,
                "The command takes no arguments."));
            mErrors++;
            return true;
        }
        mExit = true;
        return false;
    }
    if (command == "help" || command == "?" || command == "man")
    {
        PrintHelp(args);
        return true;
    }
    if (command == "echo")
    {
        /* Print the message with its original spacing: the line is
         * re-split after the command word, which keeps quoted words
         * together and everything else apart. */
        std::string rest = trimmed.substr(words[0].size());
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t'))
        {
            rest.erase(0, 1);
        }
        Console::Instance().WriteLine(rest);
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
    if (command == "lls")
    {
        ListLocal(args);
        return true;
    }
    if (command == "binary" || command == "ascii")
    {
        if (!args.empty())
        {
            Console::Instance().Error(Texts::WrongArguments(command,
                "The command takes no arguments."));
            mErrors++;
            return true;
        }
        std::vector<std::string> optionArgs;
        optionArgs.push_back("transfer");
        optionArgs.push_back(command);
        SetOption(optionArgs);
        return true;
    }
    if (command == "lpwd")
    {
        PrintLocalDirectory();
        return true;
    }
    if (command == "get" || command == "recv" || command == "mget")
    {
        GetFiles(args);
        return true;
    }
    if (command == "put" || command == "send" || command == "mput")
    {
        PutFiles(args);
        return true;
    }
    if (command == "synchronize" || command == "sync")
    {
        SynchronizeDirectories(args);
        return true;
    }
    if (command == "keepuptodate")
    {
        KeepUpToDate(args);
        return true;
    }
    if (command == "edit")
    {
        EditRemoteFile(args);
        return true;
    }
    if (command == "session" || command == "sessions")
    {
        SessionCommand(args);
        return true;
    }
    if (command == "savesession")
    {
        SaveStoredSession(args);
        return true;
    }
    if (command == "rmsession" || command == "delsession")
    {
        RemoveStoredSessionEntry(args);
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
    if (command == "cp")
    {
        CopyRemote(args);
        return true;
    }
    if (command == "ln" || command == "symlink")
    {
        LinkRemote(args, command.c_str());
        return true;
    }
    if (command == "checksum")
    {
        ChecksumRemote(args);
        return true;
    }
    if (command == "call")
    {
        CallRemote(args, "call");
        return true;
    }
    if (command == "!")
    {
        /* Upstream: '! <command>' is a synonym of 'call'. */
        CallRemote(args, "!");
        return true;
    }
    if (command == "copyid")
    {
        CopyPublicKey(args);
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
    if (command == "show")
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
    if (!mBatchConfigured)
    {
        mBatchMode = BatchMode::Abort;
        Console::Instance().SetBatchMode(true);
    }

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
        std::string expanded;
        if (ExpandParameters(line, expanded))
        {
            ExecuteLine(expanded);
        }
        else
        {
            /* A broken placeholder stops this line only; 'batch on' and
             * 'continue' still decide whether the run goes on. */
            if (mBatchMode == BatchMode::Abort || mBatchMode == BatchMode::On)
            {
                mExit = true;
            }
        }
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
    if (!mBatchConfigured)
    {
        mBatchMode = BatchMode::Abort;
        Console::Instance().SetBatchMode(true);
    }

    /* '-command=' may carry several commands separated by ';'. Each is
     * expanded on its own, so a parameter value that holds a ';' cannot
     * start a command of its own. */
    std::vector<std::string> commands = SplitCommands(line);
    for (size_t i = 0; i < commands.size(); i++)
    {
        std::string expanded;
        if (!commands[i].empty() && ExpandParameters(commands[i], expanded))
        {
            ExecuteLine(expanded);
        }
        else if (!commands[i].empty() &&
            (mBatchMode == BatchMode::Abort || mBatchMode == BatchMode::On))
        {
            mExit = true;
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
        std::string expanded;
        if (ExpandParameters(line, expanded))
        {
            ExecuteLine(expanded);
        }
    }
    return mErrors != 0 ? 1 : 0;
}

int Script::RunServer()
{
    /* The graphical client drives one of these per session: it keeps the
     * connection open between two operations instead of logging in again
     * for every listing. Errors do not end the server; the outcome of
     * every command is reported on a marker line. */
    mBatchMode = BatchMode::Continue;
    mBatchConfigured = true;
    mServerLoop = true;
    Console & console = Console::Instance();
    console.SetBatchMode(true);

    console.WriteLine("LSCP-SERVER 1 ready");

    std::string line;
    while (!mExit)
    {
        if (!console.ReadLine(std::string(), line))
        {
            break;
        }
        std::string trimmed = line;
        while (!trimmed.empty() &&
            (trimmed[trimmed.size() - 1] == '\r' || trimmed[trimmed.size() - 1] == '\n'))
        {
            trimmed.erase(trimmed.size() - 1);
        }
        if (trimmed.find_first_not_of(" \t") == std::string::npos ||
            trimmed[trimmed.find_first_not_of(" \t")] == '#')
        {
            continue;
        }
        const bool keepRunning = ExecuteLine(trimmed);
        console.WriteLine(std::string("LSCP-RESULT ") + (mLastCommandFailed ? "1" : "0"));
        if (!keepRunning)
        {
            break;
        }
    }
    return mErrors != 0 ? 1 : 0;
}

} // namespace linuxscp
