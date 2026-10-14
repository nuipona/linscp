/*
 * main: command line entry point of the front end (POSIX and Windows).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cerrno>
#include <string>
#include <vector>

#include "Console.h"
#include "PathUtil.h"
#include "Platform.h"
#include "Script.h"
#include "Session.h"
#include "Texts.h"

#if !defined(_WIN32)
#include "PuttyCompat.h"
#endif

namespace {

bool StartsWith(const char * text, const char * prefix, const char ** value)
{
    size_t len = strlen(prefix);
    if (strncmp(text, prefix, len) != 0)
    {
        return false;
    }
    if (value != NULL)
    {
        *value = text + len;
    }
    return true;
}

bool ParsePositiveInteger(const char * text, int maximum, int * result)
{
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }
    unsigned int value = 0;
    for (const unsigned char * p =
             reinterpret_cast<const unsigned char *>(text); *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return false;
        }
        const unsigned int digit = *p - '0';
        if (value > (static_cast<unsigned int>(maximum) - digit) / 10U)
        {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value == 0 || value > static_cast<unsigned int>(maximum))
    {
        return false;
    }
    *result = static_cast<int>(value);
    return true;
}

} // namespace

int main(int argc, char ** argv)
{
    linuxscp::InitConsole();
    linuxscp::Console & console = linuxscp::Console::Instance();
    linuxscp::SessionOptions options;
    std::string scriptFile;
    std::string command;
    std::string url;
    std::string logFile;
    std::string xmlLogFile;
    /* The script parameters of this run, substituted for %1% .. %9%. */
    std::vector<std::string> parameters;
    bool collectingParameters = false;
    bool batch = false;
    bool launchGui = false;
    bool serverMode = false;
    bool help = false;
    bool version = false;
    bool scriptSpecified = false;
    bool commandSpecified = false;
    const char * value = NULL;

    std::string normalizedArg;
    for (int i = 1; i < argc; i++)
    {
        const char * arg = argv[i];
#if defined(_WIN32)
        if (arg[0] == '/' && arg[1] != '\0')
        {
            static const char * known[] = {
                "script=", "command=", "hostkey=", "password=", "privatekey=",
                "username=", "port=", "timeout=", "log=", "xmllog=",
                "parameter", "parameters",
                "batch", "batch=on", "batch=off", "gui", "server", "verbose",
                "version", "help", "h"
            };
            const char * rest = arg + 1;
            for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++)
            {
                size_t len = strlen(known[k]);
                bool valueForm = len > 0 && known[k][len - 1] == '=';
                if (strncmp(rest, known[k], len) == 0 &&
                    (valueForm || rest[len] == '\0'))
                {
                    normalizedArg = std::string("-") + rest;
                    arg = normalizedArg.c_str();
                    break;
                }
            }
        }
#endif
        if (collectingParameters)
        {
            /* WinSCP takes the script parameters after a bare '/parameter';
             * they are not the site URL, however they look. */
            parameters.push_back(arg);
            continue;
        }
        if (strcmp(arg, "-parameter") == 0 || strcmp(arg, "--parameter") == 0 ||
            strcmp(arg, "-parameters") == 0)
        {
            collectingParameters = true;
        }
        else if (StartsWith(arg, "-parameter=", &value))
        {
            parameters.push_back(value);
        }
        else if (StartsWith(arg, "-script=", &value))
        {
            scriptSpecified = true;
            scriptFile = linuxscp::ExpandTilde(value);
        }
        else if (StartsWith(arg, "-command=", &value))
        {
            commandSpecified = true;
            command = value;
        }
        else if (StartsWith(arg, "-hostkey=", &value))
        {
            options.hostKey = value;
        }
        else if (StartsWith(arg, "-password=", &value))
        {
            options.password = value;
        }
        else if (StartsWith(arg, "-privatekey=", &value))
        {
            options.privateKeyFile = value;
        }
        else if (StartsWith(arg, "-username=", &value))
        {
            options.userName = value;
        }
        else if (StartsWith(arg, "-port=", &value))
        {
            if (!ParsePositiveInteger(value, 65535, &options.portNumber))
            {
                console.Error("Invalid port number '" + std::string(value) +
                    "' (expected 1 through 65535).");
                return 1;
            }
        }
        else if (StartsWith(arg, "-timeout=", &value))
        {
            if (!ParsePositiveInteger(value, INT_MAX / 1000, &options.timeoutSecs))
            {
                console.Error("Invalid timeout '" + std::string(value) +
                    "' (expected a positive number of seconds).");
                return 1;
            }
        }
        else if (StartsWith(arg, "-log=", &value))
        {
            logFile = linuxscp::ExpandTilde(value);
        }
        else if (StartsWith(arg, "-xmllog=", &value))
        {
            xmlLogFile = linuxscp::ExpandTilde(value);
        }
        else if (strcmp(arg, "-batch") == 0 || strcmp(arg, "-batch=on") == 0)
        {
            batch = true;
        }
        else if (strcmp(arg, "-batch=off") == 0)
        {
            batch = false;
        }
        else if (strcmp(arg, "-gui") == 0 || strcmp(arg, "--gui") == 0)
        {
            launchGui = true;
        }
        else if (strcmp(arg, "-server") == 0 || strcmp(arg, "--server") == 0)
        {
            serverMode = true;
        }
        else if (strcmp(arg, "-verbose") == 0)
        {
            options.verbose = true;
        }
        else if (strcmp(arg, "-version") == 0 || strcmp(arg, "--version") == 0)
        {
            version = true;
        }
        else if (strcmp(arg, "-help") == 0 || strcmp(arg, "--help") == 0 ||
            strcmp(arg, "-h") == 0)
        {
            help = true;
        }
        else if (arg[0] == '-' && arg[1] != '\0')
        {
            console.Error(std::string("Unknown option '") + arg + "'.");
            console.Error(linuxscp::Texts::Usage());
            return 1;
        }
        else if (url.empty())
        {
            url = arg;
        }
        else
        {
            console.Error(std::string("Unexpected argument '") + arg + "'.");
            return 1;
        }
    }

    if (launchGui)
    {
        std::string guiError;
        if (!linuxscp::LaunchGuiApplication(guiError))
        {
            console.Error(guiError);
            return 1;
        }
        return 0;
    }

    if (version)
    {
        console.WriteLine(linuxscp::Texts::VersionLine());
        return 0;
    }
    if (help)
    {
        console.WriteLine(linuxscp::Texts::Usage());
        return 0;
    }
    if (serverMode && (scriptSpecified || commandSpecified))
    {
        console.Error("-server reads commands from standard input and cannot "
            "be combined with -script or -command.");
        return 1;
    }
    if (scriptSpecified && commandSpecified)
    {
        console.Error("Choose either -script or -command, not both.");
        return 1;
    }
    if (scriptSpecified && scriptFile.empty())
    {
        console.Error("-script requires a file name (use -script=- for standard input).");
        return 1;
    }
    if (commandSpecified && command.empty())
    {
        console.Error("-command requires at least one command.");
        return 1;
    }
    if ((scriptSpecified || commandSpecified) && !url.empty())
    {
        console.Error("A site URL cannot be combined with -script or -command; "
            "put an 'open' command in the script instead.");
        return 1;
    }

    if (options.password.empty())
    {
        const char * password = getenv("LINUXSCP_PASSWORD");
        if (password != NULL)
        {
            options.password = password;
        }
    }

    if (!logFile.empty())
    {
        std::string error;
        if (!console.SetLogFile(logFile, error))
        {
            console.Error("Cannot open the log file '" + logFile + "': " + error);
            return 1;
        }
    }
    if (!xmlLogFile.empty())
    {
        std::string error;
        if (!console.SetActionLogFile(xmlLogFile, error))
        {
            console.Error("Cannot open the XML log file '" + xmlLogFile + "': " +
                error);
            return 1;
        }
    }

    console.SetBatchMode(batch);
    console.SetVerbose(options.verbose);

#if !defined(_WIN32)
    /* The platform layer of the PuTTY core needs these before any socket
     * or file descriptor is registered, and before any critical section
     * can be taken. */
    linuxscp_platform_init();
    sk_init();
    uxsel_init();
#endif

    linuxscp::Script script;
    script.SetDefaults(options);
    script.SetScriptParameters(parameters);

    int result;
    if (serverMode)
    {
        result = script.RunServer();
    }
    else if (scriptSpecified)
    {
        result = script.RunFile(scriptFile);
    }
    else if (commandSpecified)
    {
        result = script.RunCommand(command);
    }
    else if (!console.IsInteractive() && url.empty())
    {
        console.Error("No input. Pass -script, -command or a site URL, "
            "or run interactively from a terminal.");
        console.Error(linuxscp::Texts::Usage());
        result = 1;
    }
    else
    {
        result = script.RunInteractive(url);
    }

#if !defined(_WIN32)
    sk_cleanup();
#endif
    return result;
}
