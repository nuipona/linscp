/*
 * Platform: the operating-system services the front end needs, with a
 * POSIX and a Windows (Win32) implementation in Platform.cpp.
 *
 * Everything above this layer (PathUtil, LocalFs, Console, Script,
 * ProtocolClient, Session) is written against these helpers plus the
 * C++ standard library, so the same sources build on Linux and on
 * Windows. The PuTTY SFTP engine is the one exception: it needs the
 * PuTTY Unix platform layer (libs/putty/unix), so on Windows every
 * protocol -- SFTP included -- is served by the curl backend in
 * ProtocolClient.cpp (curl.exe ships with Windows 10/11 and supports
 * sftp/scp/ftp/ftps/http/https). See docs/windows.md.
 */

#ifndef LINUXSCP_PLATFORM_H
#define LINUXSCP_PLATFORM_H

#include <ctime>
#include <string>
#include <vector>

namespace linuxscp {

#if defined(_WIN32)
inline constexpr bool kIsWindows = true;
#else
inline constexpr bool kIsWindows = false;
#endif

/* True where the native PuTTY SFTP transport (Session + SftpClient.c +
 * PuttyGlue.cpp) is available. On Windows it is not, and SFTP goes
 * through ProtocolClient like every other protocol. */
inline constexpr bool kHasNativeSftp = !kIsWindows;

/* The preferred local directory separator ('\\' on Windows, '/'
 * elsewhere). Both separators are accepted in local paths on Windows. */
inline constexpr char kLocalPathSeparator = kIsWindows ? '\\' : '/';

/* One-time console setup (UTF-8 code page and binary standard streams
 * on Windows). Safe to call before anything is printed. */
void InitConsole();

bool StdInIsTerminal();
bool StdOutIsTerminal();

/* Turns console echo off while alive (for password prompts). Restores
 * the previous mode on destruction; a no-op when there is no terminal. */
class EchoOff
{
public:
    EchoOff();
    ~EchoOff();
    bool Active() const { return mActive; }

private:
    EchoOff(const EchoOff &);
    EchoOff & operator=(const EchoOff &);

    bool mActive;
    int mFd;        /* POSIX: descriptor the saved mode belongs to */
    void * mSaved;  /* POSIX: struct termios *, Windows: DWORD mode */
};

/* localtime_r (POSIX) / localtime_s (Windows) behind one signature. */
bool LocalTime(std::time_t value, std::tm & result);

/* The user's home directory (Windows: %USERPROFILE%, POSIX: $HOME),
 * or an empty string when it cannot be determined. */
std::string HomeDirectory();

/* Per-user configuration directory of the application, without a
 * trailing separator (Windows: %APPDATA%\LinuxSCP, POSIX:
 * $XDG_CONFIG_HOME/LinuxSCP or ~/.config/LinuxSCP). Empty when no home
 * directory is known. The directory is not created. */
std::string ConfigDirectory();

/* The current user name (Windows: %USERNAME%, POSIX: $USER), or an
 * empty string. Used as the default login name by 'open'. */
std::string CurrentUserName();

/* Sets the access/modification times (seconds since the epoch) of a
 * local file. Returns false on failure. */
bool SetFileTimes(const std::string & path, unsigned long atime,
    unsigned long mtime);

/* Parses an HTTP 'Last-Modified' header value ('Tue, 13 Oct 2026
 * 05:21:00 GMT') into seconds since the epoch. Returns 0 when the text
 * cannot be read. */
unsigned long ParseHttpDate(const std::string & text);

/* Parses the date column of an 'ls -l' entry, which is either a clock
 * time for a recent file ('Oct 13 05:21') or a year for an older one
 * ('Oct 13  2026'), into seconds since the epoch. Returns 0 when the
 * text cannot be read. */
unsigned long ParseListingDate(const std::string & monthName,
    const std::string & dayText, const std::string & timeOrYear);

/* Runs args[0] with args[1..] without a shell, capturing everything the
 * child writes to stdout and stderr (read concurrently, so a child can
 * fill both pipes without deadlocking). Returns the child's exit code,
 * or -1 when the child could not be started, in which case 'err' holds
 * the reason. Child stderr is returned in 'err' on a normal run too. */
int RunCapture(const std::vector<std::string> & args, std::string & out,
    std::string & err);

/* Full path of an executable found on PATH (applying PATHEXT on
 * Windows), or an empty string. A name containing a path separator is
 * checked as given. */
std::string FindExecutableOnPath(const std::string & name);

/* Directory containing the running executable, or an empty string. */
std::string ExecutableDirectory();

/* Starts the graphical client (linuxscp-gui). On POSIX this replaces
 * the current process and only returns on failure; on Windows it
 * starts the GUI detached and returns true. */
bool LaunchGuiApplication(std::string & error);

/* Arguments of the system 'ssh' command client that open an interactive
 * session on the given server, without the program name. Exposed for the
 * unit tests and for the message shown when no terminal can be started. */
std::vector<std::string> BuildSshArguments(const std::string & userName,
    const std::string & hostName, int portNumber,
    const std::string & privateKeyFile);

/* Opens an interactive terminal on the given server: the system ssh in a
 * desktop terminal emulator on POSIX (the terminal from LINUXSCP_TERMINAL,
 * otherwise the first of the common ones that is installed), or PuTTY,
 * Windows Terminal or the OpenSSH client on Windows. Returns false with
 * 'error' set when nothing could be started. */
bool LaunchTerminalForServer(const std::string & userName,
    const std::string & hostName, int portNumber,
    const std::string & privateKeyFile, std::string & error);

} // namespace linuxscp

#endif /* LINUXSCP_PLATFORM_H */
