#include "Platform.h"

#include "LocalFs.h"

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <sys/utime.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace linuxscp {

namespace {

std::string EnvironmentVariable(const char * name)
{
    const char * value = getenv(name);
    return value != NULL ? std::string(value) : std::string();
}

#if defined(_WIN32)

std::string WideToUtf8(const std::wstring & text)
{
    if (text.empty())
    {
        return std::string();
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), NULL, 0, NULL, NULL);
    if (len <= 0)
    {
        return std::string();
    }
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), &result[0], len, NULL, NULL);
    return result;
}

std::wstring Utf8ToWide(const std::string & text)
{
    if (text.empty())
    {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), NULL, 0);
    if (len <= 0)
    {
        /* Not valid UTF-8: fall back to the ANSI code page so that a
         * path typed in a legacy console encoding still works. */
        len = MultiByteToWideChar(CP_ACP, 0, text.data(),
            static_cast<int>(text.size()), NULL, 0);
        if (len <= 0)
        {
            return std::wstring();
        }
        std::wstring result(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(),
            static_cast<int>(text.size()), &result[0], len);
        return result;
    }
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), &result[0], len);
    return result;
}

/* Quotes one argument for a Windows command line, following the
 * CommandLineToArgvW rules used by CreateProcess consumers. */
void AppendQuotedArgument(std::wstring & commandLine, const std::string & arg)
{
    if (!commandLine.empty())
    {
        commandLine += L' ';
    }
    std::wstring wide = Utf8ToWide(arg);
    if (!wide.empty() && wide.find_first_of(L" \t\"") == std::wstring::npos)
    {
        commandLine += wide;
        return;
    }
    commandLine += L'"';
    size_t backslashes = 0;
    for (size_t i = 0; i < wide.size(); i++)
    {
        if (wide[i] == L'\\')
        {
            backslashes++;
            continue;
        }
        if (wide[i] == L'"')
        {
            commandLine.append(backslashes * 2 + 1, L'\\');
            commandLine += L'"';
        }
        else
        {
            commandLine.append(backslashes, L'\\');
            commandLine += wide[i];
        }
        backslashes = 0;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine += L'"';
}

void ReadPipeInto(HANDLE pipe, std::string & target)
{
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, NULL) && read > 0)
    {
        target.append(buffer, read);
    }
}

#endif /* _WIN32 */

} // namespace

void InitConsole()
{
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    /* Binary mode keeps '\n' as the only newline on pipes and files,
     * exactly as on POSIX, and stops Ctrl-Z from ending stdin. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != NULL && GetConsoleMode(out, &mode))
    {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

bool StdInIsTerminal()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

bool StdOutIsTerminal()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

EchoOff::EchoOff() : mActive(false), mFd(-1), mSaved(NULL)
{
#if defined(_WIN32)
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != NULL && GetConsoleMode(input, &mode))
    {
        if (SetConsoleMode(input, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)))
        {
            mSaved = reinterpret_cast<void *>(static_cast<uintptr_t>(mode));
            mActive = true;
        }
    }
#else
    if (isatty(STDIN_FILENO))
    {
        struct termios * saved = new struct termios;
        if (tcgetattr(STDIN_FILENO, saved) == 0)
        {
            struct termios muted = *saved;
            muted.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            if (tcsetattr(STDIN_FILENO, TCSANOW, &muted) == 0)
            {
                mFd = STDIN_FILENO;
                mSaved = saved;
                mActive = true;
            }
        }
        if (!mActive)
        {
            delete saved;
        }
    }
#endif
}

EchoOff::~EchoOff()
{
#if defined(_WIN32)
    if (mActive)
    {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input != NULL)
        {
            SetConsoleMode(input,
                static_cast<DWORD>(reinterpret_cast<uintptr_t>(mSaved)));
        }
    }
#else
    if (mActive && mSaved != NULL)
    {
        tcsetattr(mFd, TCSANOW, static_cast<struct termios *>(mSaved));
        delete static_cast<struct termios *>(mSaved);
    }
#endif
}

bool LocalTime(std::time_t value, std::tm & result)
{
#if defined(_WIN32)
    return localtime_s(&result, &value) == 0;
#else
    return localtime_r(&value, &result) != NULL;
#endif
}

std::string HomeDirectory()
{
#if defined(_WIN32)
    std::string profile = EnvironmentVariable("USERPROFILE");
    if (!profile.empty())
    {
        return profile;
    }
    std::string drive = EnvironmentVariable("HOMEDRIVE");
    std::string path = EnvironmentVariable("HOMEPATH");
    if (!drive.empty() && !path.empty())
    {
        return drive + path;
    }
    return EnvironmentVariable("HOME");
#else
    return EnvironmentVariable("HOME");
#endif
}

std::string ConfigDirectory()
{
#if defined(_WIN32)
    std::string overrideDir = EnvironmentVariable("LINUXSCP_CONFIG_DIR");
    if (!overrideDir.empty())
    {
        while (overrideDir.size() > 1 && (overrideDir[overrideDir.size() - 1] == '\\' ||
            overrideDir[overrideDir.size() - 1] == '/'))
        {
            overrideDir.erase(overrideDir.size() - 1);
        }
        return overrideDir;
    }
    std::string appData = EnvironmentVariable("APPDATA");
    if (!appData.empty())
    {
        return appData + "\\LinuxSCP";
    }
    std::string home = HomeDirectory();
    if (!home.empty())
    {
        return home + "\\AppData\\Roaming\\LinuxSCP";
    }
    return std::string();
#else
    std::string overrideDir = EnvironmentVariable("LINUXSCP_CONFIG_DIR");
    if (!overrideDir.empty())
    {
        return overrideDir;
    }
    std::string xdg = EnvironmentVariable("XDG_CONFIG_HOME");
    if (!xdg.empty())
    {
        return xdg + "/LinuxSCP";
    }
    std::string home = HomeDirectory();
    if (!home.empty())
    {
        return home + "/.config/LinuxSCP";
    }
    return std::string();
#endif
}

std::string CurrentUserName()
{
#if defined(_WIN32)
    std::string user = EnvironmentVariable("USERNAME");
    if (!user.empty())
    {
        return user;
    }
    return EnvironmentVariable("USER");
#else
    std::string user = EnvironmentVariable("USER");
    if (!user.empty())
    {
        return user;
    }
    return EnvironmentVariable("LOGNAME");
#endif
}

bool SetFileTimes(const std::string & path, unsigned long atime,
    unsigned long mtime)
{
#if defined(_WIN32)
    struct _utimbuf times;
    times.actime = static_cast<time_t>(atime);
    times.modtime = static_cast<time_t>(mtime);
    return _utime(path.c_str(), &times) == 0;
#else
    struct timeval values[2];
    values[0].tv_sec = static_cast<time_t>(atime);
    values[0].tv_usec = 0;
    values[1].tv_sec = static_cast<time_t>(mtime);
    values[1].tv_usec = 0;
    return utimes(path.c_str(), values) == 0;
#endif
}

std::string TempDirectory()
{
#if defined(_WIN32)
    wchar_t buffer[32768];
    DWORD len = GetTempPathW(32768, buffer);
    if (len == 0 || len >= 32768)
    {
        return std::string();
    }
    std::string path = WideToUtf8(std::wstring(buffer, len));
    while (!path.empty() && (path[path.size() - 1] == '\\' || path[path.size() - 1] == '/'))
    {
        path.erase(path.size() - 1);
    }
    return path;
#else
    const char * candidates[] = { "TMPDIR", "TEMP", "TMP" };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
    {
        std::string value = EnvironmentVariable(candidates[i]);
        if (!value.empty())
        {
            while (value.size() > 1 && value[value.size() - 1] == '/')
            {
                value.erase(value.size() - 1);
            }
            return value;
        }
    }
    return "/tmp";
#endif
}

bool CreatePrivateDirectory(const std::string & path, std::string & error)
{
    /* The parents of these directories are the user's own configuration
     * or temporary directory, which other users cannot reach; only the
     * leaf is restricted explicitly. */
    if (!CreateLocalDirectoryRecursive(path, error))
    {
        return false;
    }
#if !defined(_WIN32)
    chmod(path.c_str(), 0700);
#endif
    return true;
}

bool RestrictFileToUser(const std::string & path)
{
#if defined(_WIN32)
    /* The profile and temporary directories already keep other users
     * out; there is no portable ACL edit here. */
    (void)path;
    return true;
#else
    return chmod(path.c_str(), 0600) == 0;
#endif
}

void SleepMilliseconds(unsigned milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

namespace {

volatile sig_atomic_t gInterrupted = 0;

#if !defined(_WIN32)
void InterruptHandler(int)
{
    gInterrupted = 1;
}
#else
BOOL WINAPI ConsoleInterruptHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT)
    {
        gInterrupted = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

} // namespace

void InstallInterruptHandler()
{
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;
#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleInterruptHandler, TRUE);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = InterruptHandler;
    sigaction(SIGINT, &action, NULL);
#endif
}

bool Interrupted()
{
    return gInterrupted != 0;
}

int RunInteractive(const std::vector<std::string> & args, std::string & error)
{
    if (args.empty())
    {
        error = "No program to run.";
        return -1;
    }
#if defined(_WIN32)
    std::wstring commandLine;
    for (size_t i = 0; i < args.size(); i++)
    {
        AppendQuotedArgument(commandLine, args[i]);
    }
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    if (CreateProcessW(NULL, mutableCommand.data(), NULL, NULL, TRUE, 0,
            NULL, NULL, &startup, &process) == 0)
    {
        error = std::string("Cannot run '") + args[0] + "'.";
        return -1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
    {
        error = std::string("Cannot start '") + args[0] + "': " + strerror(errno);
        return -1;
    }
    if (pid == 0)
    {
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
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            error = std::string("Cannot wait for '") + args[0] + "': " + strerror(errno);
            return -1;
        }
    }
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return -1;
#endif
}

int RunCapture(const std::vector<std::string> & args, std::string & out,
    std::string & err)
{
    out.clear();
    err.clear();
    if (args.empty() || args[0].empty())
    {
        err = "No command to run.";
        return -1;
    }

#if defined(_WIN32)
    std::wstring commandLine;
    for (size_t i = 0; i < args.size(); i++)
    {
        AppendQuotedArgument(commandLine, args[i]);
    }

    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE outRead = NULL, outWrite = NULL, errRead = NULL, errWrite = NULL;
    if (!CreatePipe(&outRead, &outWrite, &security, 0) ||
        !CreatePipe(&errRead, &errWrite, &security, 0))
    {
        err = "Failed to create pipes (Windows error " +
            std::to_string(GetLastError()) + ").";
        if (outRead != NULL) CloseHandle(outRead);
        if (outWrite != NULL) CloseHandle(outWrite);
        if (errRead != NULL) CloseHandle(errRead);
        if (errWrite != NULL) CloseHandle(errWrite);
        return -1;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');
    BOOL created = CreateProcessW(NULL, commandBuffer.data(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!created)
    {
        DWORD code = GetLastError();
        CloseHandle(outRead);
        CloseHandle(errRead);
        err = "Failed to start '" + args[0] + "' (Windows error " +
            std::to_string(code) + ").";
        return -1;
    }

    /* Read both pipes concurrently: a child that fills the stderr pipe
     * while we drain stdout (or vice versa) would otherwise deadlock. */
    std::thread outThread(ReadPipeInto, outRead, std::ref(out));
    std::thread errThread(ReadPipeInto, errRead, std::ref(err));
    outThread.join();
    errThread.join();
    CloseHandle(outRead);
    CloseHandle(errRead);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
#else
    int outPipe[2];
    int errPipe[2];
    if (pipe(outPipe) != 0)
    {
        err = std::string("Failed to create stdout pipe: ") + strerror(errno);
        return -1;
    }
    if (pipe(errPipe) != 0)
    {
        close(outPipe[0]);
        close(outPipe[1]);
        err = std::string("Failed to create stderr pipe: ") + strerror(errno);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        err = std::string("Failed to fork: ") + strerror(errno);
        return -1;
    }

    if (pid == 0)
    {
        close(outPipe[0]);
        close(errPipe[0]);
        if (dup2(outPipe[1], STDOUT_FILENO) < 0 ||
            dup2(errPipe[1], STDERR_FILENO) < 0)
        {
            _exit(127);
        }
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

    close(outPipe[1]);
    close(errPipe[1]);

    /* Drain both pipes with poll() so the child never blocks on a full
     * pipe while we are reading the other one. */
    struct pollfd fds[2];
    fds[0].fd = outPipe[0];
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = errPipe[0];
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    int openCount = 2;
    char buffer[4096];
    while (openCount > 0)
    {
        int ready = poll(fds, 2, -1);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        for (int i = 0; i < 2; i++)
        {
            if (fds[i].fd < 0)
            {
                continue;
            }
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                ssize_t bytesRead = read(fds[i].fd, buffer, sizeof(buffer));
                if (bytesRead > 0)
                {
                    (i == 0 ? out : err).append(buffer,
                        static_cast<size_t>(bytesRead));
                }
                else
                {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    openCount--;
                }
            }
            fds[i].revents = 0;
        }
    }
    if (fds[0].fd >= 0)
    {
        close(fds[0].fd);
    }
    if (fds[1].fd >= 0)
    {
        close(fds[1].fd);
    }

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
    if (WIFSIGNALED(status))
    {
        return 128 + WTERMSIG(status);
    }
    return -1;
#endif
}

std::string FindExecutableOnPath(const std::string & name)
{
    if (name.empty())
    {
        return std::string();
    }
#if defined(_WIN32)
    std::wstring wide = Utf8ToWide(name);
    wchar_t buffer[32768];
    DWORD len = SearchPathW(NULL, wide.c_str(), L".exe", 32768, buffer, NULL);
    if (len == 0 || len >= 32768)
    {
        return std::string();
    }
    return WideToUtf8(std::wstring(buffer, len));
#else
    if (name.find('/') != std::string::npos)
    {
        return access(name.c_str(), X_OK) == 0 ? name : std::string();
    }
    const char * pathEnv = getenv("PATH");
    if (pathEnv == NULL)
    {
        return std::string();
    }
    std::string path = pathEnv;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start,
            colon == std::string::npos ? colon : colon - start);
        if (dir.empty())
        {
            dir = ".";
        }
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
        if (colon == std::string::npos)
        {
            break;
        }
        start = colon + 1;
    }
    return std::string();
#endif
}

std::string ExecutableDirectory()
{
#if defined(_WIN32)
    wchar_t buffer[32768];
    DWORD len = GetModuleFileNameW(NULL, buffer, 32768);
    if (len == 0 || len >= 32768)
    {
        return std::string();
    }
    std::string full = WideToUtf8(std::wstring(buffer, len));
    std::string::size_type slash = full.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : full.substr(0, slash);
#else
    char buffer[4096];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0)
    {
        return std::string();
    }
    buffer[len] = '\0';
    std::string full(buffer, static_cast<size_t>(len));
    std::string::size_type slash = full.find_last_of('/');
    return slash == std::string::npos ? std::string() : full.substr(0, slash);
#endif
}

bool LaunchGuiApplication(std::string & error)
{
    error.clear();
#if defined(_WIN32)
    std::string dir = ExecutableDirectory();
    const char * names[] = { "linuxscp-gui.exe", "linuxscp_gui.py",
        "linuxscp-gui.py" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        std::string candidate = dir.empty() ? names[i] : dir + "\\" + names[i];
        if (GetFileAttributesA(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }
        std::wstring wide = Utf8ToWide(candidate);
        HINSTANCE result = ShellExecuteW(NULL, L"open", wide.c_str(), NULL,
            dir.empty() ? NULL : Utf8ToWide(dir).c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32)
        {
            return true;
        }
        error = "Failed to launch '" + candidate + "'.";
        return false;
    }
    error = "Cannot find linuxscp-gui.exe or linuxscp_gui.py next to "
        "linuxscp.exe. Install the GUI files alongside the console client, "
        "or run 'python linuxscp_gui.py' from the source/gui directory.";
    return false;
#else
    const char * guiBin = "/usr/bin/linuxscp-gui";
    if (access(guiBin, X_OK) != 0)
    {
        guiBin = "linuxscp-gui";
    }
    execlp(guiBin, guiBin, static_cast<char *>(NULL));
    error = std::string("Failed to launch LinuxSCP GUI: ") + strerror(errno);
    return false;
#endif
}

/*
 * The date columns of the protocol backends: curl's 'Last-Modified'
 * header and the date column of an 'ls -l' listing. Both live here so
 * that the unit tests can reach them without the PuTTY core.
 */

static int MonthFromName(const std::string & name)
{
    static const char * const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (int i = 0; i < 12; i++)
    {
        if (name == months[i])
        {
            return i;
        }
    }
    return -1;
}

unsigned long ParseHttpDate(const std::string & text)
{
    /* 'Tue, 13 Oct 2026 05:21:00 GMT', as curl prints Last-Modified. */
    const char * start = strchr(text.c_str(), ',');
    start = start != NULL ? start + 1 : text.c_str();
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char monthName[16] = {0};
    if (sscanf(start, " %d %15s %d %d:%d:%d", &day, monthName, &year, &hour,
            &minute, &second) != 6)
    {
        return 0;
    }
    const int month = MonthFromName(monthName);
    if (month < 0 || day <= 0 || day > 31)
    {
        return 0;
    }
    struct tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    /* The header is in GMT. */
#if defined(_WIN32)
    const std::time_t stamp = _mkgmtime(&value);
#else
    const std::time_t stamp = timegm(&value);
#endif
    return stamp > 0 ? static_cast<unsigned long>(stamp) : 0;
}

unsigned long ParseListingDate(const std::string & monthName,
    const std::string & dayText, const std::string & timeOrYear)
{
    /*
     * 'Oct 13 05:21' for an entry from the last six months, 'Oct 13  2026'
     * for an older one. The listing uses the server's local time, which is
     * the same clock the client displays timestamps in.
     */
    const int month = MonthFromName(monthName);
    int day = 0;
    if (month < 0 || dayText.empty())
    {
        return 0;
    }
    try
    {
        day = std::stoi(dayText);
    }
    catch (...)
    {
        return 0;
    }
    if (day <= 0 || day > 31)
    {
        return 0;
    }

    const bool hasClockTime = timeOrYear.find(':') != std::string::npos;
    int hour = 0;
    int minute = 0;
    int year = 0;
    if (hasClockTime)
    {
        const std::string::size_type colon = timeOrYear.find(':');
        try
        {
            hour = std::stoi(timeOrYear.substr(0, colon));
            minute = std::stoi(timeOrYear.substr(colon + 1));
        }
        catch (...)
        {
            return 0;
        }
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        {
            return 0;
        }
        std::time_t now = std::time(NULL);
        struct tm nowParts;
        if (!LocalTime(now, nowParts))
        {
            return 0;
        }
        year = nowParts.tm_year + 1900;
    }
    else
    {
        try
        {
            year = std::stoi(timeOrYear);
        }
        catch (...)
        {
            return 0;
        }
    }
    if (year < 1970 || year > 9999)
    {
        return 0;
    }

    struct tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = 0;
    value.tm_isdst = -1;
#if defined(_WIN32)
    std::time_t stamp = _mktime64(&value);
#else
    std::time_t stamp = std::mktime(&value);
#endif
    if (hasClockTime)
    {
        /* No year in the listing: a date more than a day ahead of the
         * clock must belong to the previous year. */
        const std::time_t limit = std::time(NULL) + 24 * 60 * 60;
        if (stamp > limit)
        {
            value.tm_year--;
#if defined(_WIN32)
            stamp = _mktime64(&value);
#else
            stamp = std::mktime(&value);
#endif
        }
    }
    return stamp > 0 ? static_cast<unsigned long>(stamp) : 0;
}


/* ----------------------------------------------------------------------
 * An interactive terminal on the connected server
 *
 * WinSCP's "Open Terminal" opens the desktop's terminal program on the
 * server that the session is using. The console client cannot make its
 * own terminal window, so it hands the same job to the system ssh client
 * (or, on Windows, to PuTTY or Windows Terminal) in a terminal emulator.
 */

std::vector<std::string> BuildSshArguments(const std::string & userName,
    const std::string & hostName, int portNumber,
    const std::string & privateKeyFile)
{
    std::vector<std::string> args;
    /* The system ssh client reads OpenSSH key files but cannot read the
     * PuTTY .ppk files that this client's own SSH core uses; a .ppk is
     * therefore left to the agent (the terminal then asks for a password
     * when there is no usable agent key). */
    std::string keyExtension = privateKeyFile.size() >= 4 ?
        privateKeyFile.substr(privateKeyFile.size() - 4) : std::string();
    for (size_t i = 0; i < keyExtension.size(); i++)
    {
        keyExtension[i] = static_cast<char>(tolower(static_cast<unsigned char>(keyExtension[i])));
    }
    if (!privateKeyFile.empty() && keyExtension != ".ppk")
    {
        args.push_back("-i");
        args.push_back(privateKeyFile);
    }
    if (portNumber > 0 && portNumber != 22)
    {
        args.push_back("-p");
        args.push_back(std::to_string(portNumber));
    }
    /* A terminal is wanted even though the client's own stdin is not one
     * (in the GUI the client runs without a console). */
    args.push_back("-t");
    args.push_back(userName.empty() ? hostName : userName + "@" + hostName);
    return args;
}

#if defined(_WIN32)

namespace {

/* PuTTY only accepts its own key files, so an OpenSSH key file must not
 * be handed to it through -i. */
bool LooksLikePpkFile(const std::string & path)
{
    FILE * file = fopen(path.c_str(), "rb");
    if (file == NULL)
    {
        return false;
    }
    char signature[21] = {0};
    const size_t got = fread(signature, 1, 20, file);
    fclose(file);
    return got == 20 && strncmp(signature, "PuTTY-User-Key-File-", 20) == 0;
}

std::string QuoteWindowsArgument(const std::string & text)
{
    std::string quoted = "\"";
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '"')
        {
            quoted += '"';
        }
        quoted += text[i];
    }
    quoted += '"';
    return quoted;
}

bool ShellExecuteProgram(const std::string & program,
    const std::string & parameters, std::string & error)
{
    HINSTANCE result = ShellExecuteA(NULL, "open", program.c_str(),
        parameters.c_str(), NULL, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        error = "Failed to start '" + program + "' (Windows error " +
            std::to_string(static_cast<long>(reinterpret_cast<INT_PTR>(result))) + ").";
        return false;
    }
    return true;
}

} // namespace

bool LaunchTerminalForServer(const std::string & userName,
    const std::string & hostName, int portNumber,
    const std::string & privateKeyFile, std::string & error)
{
    error.clear();
    const std::string target = userName.empty() ? hostName :
        userName + "@" + hostName;

    /* PuTTY first, like WinSCP: it is what a user with PuTTY installed
     * expects the button to open. */
    const std::string putty = FindExecutableOnPath("putty");
    if (!putty.empty())
    {
        std::string parameters = "-ssh " + QuoteWindowsArgument(target);
        if (portNumber > 0 && portNumber != 22)
        {
            parameters += " -P " + std::to_string(portNumber);
        }
        if (!privateKeyFile.empty() && LooksLikePpkFile(privateKeyFile))
        {
            parameters += " -i " + QuoteWindowsArgument(privateKeyFile);
        }
        return ShellExecuteProgram(putty, parameters, error);
    }

    const std::vector<std::string> sshArgs = BuildSshArguments(userName,
        hostName, portNumber, privateKeyFile);
    std::string sshCommand;
    for (size_t i = 0; i < sshArgs.size(); i++)
    {
        if (i > 0)
        {
            sshCommand += " ";
        }
        sshCommand += QuoteWindowsArgument(sshArgs[i]);
    }

    const std::string windowsTerminal = FindExecutableOnPath("wt");
    if (!windowsTerminal.empty())
    {
        return ShellExecuteProgram(windowsTerminal, "ssh " + sshCommand, error);
    }

    const std::string ssh = FindExecutableOnPath("ssh");
    if (!ssh.empty())
    {
        return ShellExecuteProgram("cmd.exe", "/k " + sshCommand, error);
    }

    error = "No terminal program was found: install PuTTY, Windows Terminal or "
        "the OpenSSH client (ssh.exe), or run 'ssh " +
        (userName.empty() ? hostName : userName + "@" + hostName) +
        "' in a terminal of your own.";
    return false;
}

#else /* POSIX */

namespace {

struct TerminalProgram
{
    const char * name;
    const char * leadingArgs[3];
};

/*
 * The terminal emulators of the common desktops, with the option that
 * makes them run a command instead of a shell. Some of them take the
 * command after '--', some after '-e' or '-x', and kitty/foot take it
 * directly.
 */
const TerminalProgram kTerminalPrograms[] = {
    {"x-terminal-emulator", {"-e", NULL}},
    {"gnome-terminal", {"--", NULL}},
    {"konsole", {"-e", NULL}},
    {"xfce4-terminal", {"-x", NULL}},
    {"mate-terminal", {"-x", NULL}},
    {"lxterminal", {"-e", NULL}},
    {"terminator", {"-x", NULL}},
    {"alacritty", {"-e", NULL}},
    {"kitty", {NULL}},
    {"foot", {NULL}},
    {"wezterm", {"start", "--", NULL}},
    {"urxvt", {"-e", NULL}},
    {"xterm", {"-e", NULL}},
};

std::vector<std::string> SplitWhitespace(const std::string & text)
{
    std::vector<std::string> words;
    std::string word;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == ' ' || text[i] == '\t')
        {
            if (!word.empty())
            {
                words.push_back(word);
                word.clear();
            }
            continue;
        }
        word += text[i];
    }
    if (!word.empty())
    {
        words.push_back(word);
    }
    return words;
}

std::string SshHint(const std::string & userName, const std::string & hostName,
    int portNumber)
{
    std::string hint = " Run 'ssh";
    if (portNumber > 0 && portNumber != 22)
    {
        hint += " -p " + std::to_string(portNumber);
    }
    hint += " " + (userName.empty() ? hostName : userName + "@" + hostName);
    hint += "' in a terminal of your own instead.";
    return hint;
}

} // namespace

bool LaunchTerminalForServer(const std::string & userName,
    const std::string & hostName, int portNumber,
    const std::string & privateKeyFile, std::string & error)
{
    error.clear();

    std::string program;
    std::vector<std::string> leading;
    const std::string overrideProgram = EnvironmentVariable("LINUXSCP_TERMINAL");
    if (!overrideProgram.empty())
    {
        /* '<program> [args]', e.g. 'xterm -e' or 'foot'. */
        const std::vector<std::string> words = SplitWhitespace(overrideProgram);
        program = words[0];
        leading.assign(words.begin() + 1, words.end());
    }
    else
    {
        if (EnvironmentVariable("DISPLAY").empty() &&
            EnvironmentVariable("WAYLAND_DISPLAY").empty())
        {
            error = "No graphical session was found (DISPLAY and WAYLAND_DISPLAY "
                "are not set), so no terminal window can be opened.";
            error += SshHint(userName, hostName, portNumber);
            return false;
        }
        for (size_t i = 0; i < sizeof(kTerminalPrograms) / sizeof(kTerminalPrograms[0]); i++)
        {
            const std::string found = FindExecutableOnPath(kTerminalPrograms[i].name);
            if (found.empty())
            {
                continue;
            }
            program = found;
            for (int n = 0; n < 3 && kTerminalPrograms[i].leadingArgs[n] != NULL; n++)
            {
                leading.push_back(kTerminalPrograms[i].leadingArgs[n]);
            }
            break;
        }
        if (program.empty())
        {
            error = "None of the common desktop terminals is installed; set "
                "LINUXSCP_TERMINAL to the terminal program to use.";
            error += SshHint(userName, hostName, portNumber);
            return false;
        }
    }

    std::string ssh = FindExecutableOnPath("ssh");
    if (ssh.empty())
    {
        error = "The ssh client is not installed, so no terminal can be opened.";
        return false;
    }

    std::vector<std::string> command;
    command.push_back(program);
    command.insert(command.end(), leading.begin(), leading.end());
    command.push_back(ssh);
    const std::vector<std::string> sshArgs = BuildSshArguments(userName,
        hostName, portNumber, privateKeyFile);
    command.insert(command.end(), sshArgs.begin(), sshArgs.end());

    /*
     * Start the terminal without waiting for it (it outlives this
     * process), but report a failed exec: the write end of the pipe is
     * closed by the exec, and a child that cannot exec writes the reason
     * into it instead.
     */
    int statusPipe[2];
    if (pipe(statusPipe) != 0)
    {
        error = std::string("Failed to create the exec status pipe: ") + strerror(errno);
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0)
    {
        close(statusPipe[0]);
        close(statusPipe[1]);
        error = std::string("Failed to fork: ") + strerror(errno);
        return false;
    }
    if (pid == 0)
    {
        /* Intermediate child: exit at once so the terminal is not a child
         * of this process (nobody would wait for it). */
        close(statusPipe[0]);
        const pid_t grandchild = fork();
        if (grandchild != 0)
        {
            _exit(grandchild < 0 ? 1 : 0);
        }
        (void)fcntl(statusPipe[1], F_SETFD, FD_CLOEXEC);
        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (size_t i = 0; i < command.size(); i++)
        {
            argv.push_back(const_cast<char *>(command[i].c_str()));
        }
        argv.push_back(NULL);
        execvp(argv[0], argv.data());
        const int saved = errno;
        (void)!write(statusPipe[1], &saved, sizeof(saved));
        _exit(127);
    }

    close(statusPipe[1]);
    int childStatus = 0;
    while (waitpid(pid, &childStatus, 0) < 0 && errno == EINTR)
    {
    }
    int execError = 0;
    const ssize_t got = read(statusPipe[0], &execError, sizeof(execError));
    close(statusPipe[0]);
    if (got == static_cast<ssize_t>(sizeof(execError)) && execError != 0)
    {
        error = "Failed to start '" + program + "': " + strerror(execError) + ".";
        error += SshHint(userName, hostName, portNumber);
        return false;
    }
    return true;
}

#endif /* _WIN32 */
} // namespace linuxscp
