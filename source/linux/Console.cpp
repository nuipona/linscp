#include "Console.h"

#include <cerrno>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace linuxscp {

Console & Console::Instance()
{
    static Console console;
    return console;
}

Console::Console() :
    mBatchMode(false),
    mVerbose(false),
    mLog(NULL),
    mTransferSize(0),
    mLastPermille(0)
{
}

Console::~Console()
{
    CloseLogFile();
}

bool Console::IsInteractive() const
{
    return !mBatchMode && (isatty(STDIN_FILENO) != 0);
}

void Console::Write(const std::string & text)
{
    fputs(text.c_str(), stdout);
    fflush(stdout);
}

void Console::WriteLine(const std::string & text)
{
    fputs(text.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void Console::Status(const std::string & text)
{
    WriteLine(text);
}

void Console::Error(const std::string & text)
{
    fputs(text.c_str(), stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

void Console::Fatal(const std::string & text)
{
    Error(text);
}

void Console::LogEvent(const std::string & text)
{
    if (mVerbose)
    {
        WriteLine(text);
    }
    if (mLog != NULL)
    {
        char stamp[64];
        time_t now = time(NULL);
        struct tm tmResult;
        localtime_r(&now, &tmResult);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmResult);
        fprintf(mLog, ". %s Event: %s\n", stamp, text.c_str());
        fflush(mLog);
    }
}

bool Console::ReadLine(const std::string & prompt, std::string & line)
{
    if (!prompt.empty())
    {
        Write(prompt);
    }
    line.clear();
    int c;
    bool any = false;
    while ((c = fgetc(stdin)) != EOF)
    {
        any = true;
        if (c == '\n')
        {
            return true;
        }
        if (c != '\r')
        {
            line += static_cast<char>(c);
        }
    }
    if (any && !line.empty())
    {
        return true;
    }
    return false;
}

bool Console::ReadPassword(const std::string & prompt, std::string & password)
{
    password.clear();
    struct termios oldTermios;
    bool echoOff = false;
    if (isatty(STDIN_FILENO) && (tcgetattr(STDIN_FILENO, &oldTermios) == 0))
    {
        struct termios newTermios = oldTermios;
        newTermios.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &newTermios) == 0)
        {
            echoOff = true;
        }
    }

    Write(prompt);
    bool result = false;
    int c;
    while ((c = fgetc(stdin)) != EOF)
    {
        if (c == '\n')
        {
            result = true;
            break;
        }
        if (c != '\r')
        {
            password += static_cast<char>(c);
        }
    }

    if (echoOff)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldTermios);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return result;
}

bool Console::Confirm(const std::string & prompt, bool defaultYes)
{
    if (mBatchMode)
    {
        return defaultYes;
    }
    std::string answer;
    std::string full = prompt + (defaultYes ? " [Y/n]: " : " [y/N]: ");
    if (!ReadLine(full, answer))
    {
        return defaultYes;
    }
    if (answer.empty())
    {
        return defaultYes;
    }
    return (answer[0] == 'y' || answer[0] == 'Y');
}

bool Console::SetLogFile(const std::string & path, std::string & error)
{
    CloseLogFile();
    mLog = fopen(path.c_str(), "a");
    if (mLog == NULL)
    {
        error = std::string(strerror(errno));
        return false;
    }
    char stamp[64];
    time_t now = time(NULL);
    struct tm tmResult;
    localtime_r(&now, &tmResult);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmResult);
    fprintf(mLog, "= session opened %s =\n", stamp);
    fflush(mLog);
    return true;
}

void Console::LogTraffic(const void * data, size_t len, bool incoming)
{
    if (mLog == NULL || len == 0)
    {
        return;
    }
    fputc(incoming ? '<' : '>', mLog);
    fputc(' ', mLog);
    fwrite(data, 1, len, mLog);
    fflush(mLog);
}

void Console::CloseLogFile()
{
    if (mLog != NULL)
    {
        fputs("= session closed =\n", mLog);
        fclose(mLog);
        mLog = NULL;
    }
}

void Console::TransferStart(const std::string & fileName, uint64_t totalSize)
{
    mTransferSize = totalSize;
    mLastPermille = 0;
    if (mBatchMode || !isatty(STDOUT_FILENO))
    {
        return;
    }
    Write(fileName);
    if (totalSize > 0)
    {
        size_t pad = fileName.size() < 24 ? 24 - fileName.size() : 1;
        for (size_t i = 0; i < pad; i++)
        {
            fputc(' ', stdout);
        }
        fputs("|          0.0%", stdout);
    }
    fflush(stdout);
}

void Console::TransferProgress(uint64_t transferred)
{
    if (mBatchMode || !isatty(STDOUT_FILENO) || mTransferSize == 0)
    {
        return;
    }
    unsigned long long permille =
        static_cast<unsigned long long>((transferred * 1000) / mTransferSize);
    if (permille > 1000)
    {
        permille = 1000;
    }
    if (permille == mLastPermille)
    {
        return;
    }
    mLastPermille = permille;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "\r|  %3llu.%llu%%    ",
        permille / 10, permille % 10);
    fputs(buffer, stdout);
    fflush(stdout);
}

void Console::TransferEnd(bool success)
{
    if (mBatchMode || !isatty(STDOUT_FILENO) || mTransferSize == 0)
    {
        return;
    }
    fputs(success ? "\r|  100.0%    \n" : "\n", stdout);
    fflush(stdout);
    mTransferSize = 0;
}

} // namespace linuxscp
