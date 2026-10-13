#include "Platform.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

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

} // namespace linuxscp
