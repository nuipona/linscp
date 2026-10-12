/*
 * main: command line entry point of the POSIX front end.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Console.h"
#include "Script.h"
#include "Session.h"
#include "Texts.h"

#include "PuttyCompat.h"

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

} // namespace

int main(int argc, char ** argv)
{
    linuxscp::Console & console = linuxscp::Console::Instance();
    linuxscp::SessionOptions options;
    std::string scriptFile;
    std::string command;
    std::string url;
    std::string logFile;
    bool batch = false;
    bool help = false;
    bool version = false;
    const char * value = NULL;

    for (int i = 1; i < argc; i++)
    {
        const char * arg = argv[i];
        if (StartsWith(arg, "-script=", &value))
        {
            scriptFile = value;
        }
        else if (StartsWith(arg, "-command=", &value))
        {
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
            options.portNumber = atoi(value);
        }
        else if (StartsWith(arg, "-timeout=", &value))
        {
            options.timeoutSecs = atoi(value);
        }
        else if (StartsWith(arg, "-log=", &value))
        {
            logFile = value;
        }
        else if (strcmp(arg, "-batch") == 0)
        {
            batch = true;
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

    console.SetBatchMode(batch);
    console.SetVerbose(options.verbose);

    /* The platform layer of the PuTTY core needs these before any socket
     * or file descriptor is registered, and before any critical section
     * can be taken. */
    linuxscp_platform_init();
    sk_init();
    uxsel_init();

    linuxscp::Script script;
    script.SetDefaults(options);

    int result;
    if (!scriptFile.empty())
    {
        result = script.RunFile(scriptFile);
    }
    else if (!command.empty())
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

    sk_cleanup();
    return result;
}
