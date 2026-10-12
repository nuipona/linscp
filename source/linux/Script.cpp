#include "Script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

} // namespace

Script::Script() :
    mConfirm(true),
    mExit(false),
    mErrors(0)
{
}

bool Script::RequireConnection()
{
    if (!mSession.IsOpen())
    {
        Console::Instance().Error(Texts::NotConnected());
        mErrors++;
        return false;
    }
    return true;
}

bool Script::OpenSite(const std::vector<std::string> & args)
{
    SessionOptions options = mDefaults;
    options.hostName.clear();
    options.portNumber = 22;
    options.userName.clear();
    options.password.clear();
    options.hostKey = mDefaults.hostKey;
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
    }

    if (url.empty())
    {
        Console::Instance().Error(Texts::WrongArguments("open",
            "Syntax: open sftp://[user[:password]@]host[:port]/[path]"));
        mErrors++;
        return false;
    }

    std::string error;
    SessionOptions parsed;
    if (!ParseSiteUrl(url, parsed, error))
    {
        Console::Instance().Error(error);
        mErrors++;
        return false;
    }

    options.hostName = parsed.hostName;
    options.portNumber = parsed.portNumber;
    options.initialDirectory = parsed.initialDirectory;
    if (!parsed.userName.empty())
    {
        options.userName = parsed.userName;
    }
    if (!parsed.password.empty())
    {
        options.password = parsed.password;
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
    for (int i = 0; i < names->count; i++)
    {
        std::string name = names->names[i].filename != NULL ?
            names->names[i].filename : "";
        PrintRemoteEntry(name, names->names[i], longFormat);
    }
    int count = names->count;
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

bool Script::GetFiles(const std::vector<std::string> & args)
{
    if (!RequireConnection())
    {
        return false;
    }
    if (args.size() < 2)
    {
        Console::Instance().Error(Texts::WrongArguments("get",
            "Syntax: get <remotefile(s)> <localpath>"));
        mErrors++;
        return false;
    }
    std::string destination = args[args.size() - 1];
    LocalFileInfo destinationInfo = StatLocal(ExpandTilde(destination));
    bool destinationIsDir = destinationInfo.isDir;

    bool any = false;
    for (size_t i = 0; i + 1 < args.size(); i++)
    {
        std::vector<std::string> sources;
        std::string error;
        if (!ExpandRemote(args[i], sources, error))
        {
            Console::Instance().Error(Texts::RemoteError("read", args[i], error));
            mErrors++;
            continue;
        }
        if (sources.empty())
        {
            Console::Instance().Error(Texts::RemoteError("find", args[i],
                "No such file."));
            mErrors++;
            continue;
        }
        for (size_t n = 0; n < sources.size(); n++)
        {
            std::string localPath;
            if (destinationIsDir || sources.size() > 1 || args.size() > 2)
            {
                localPath = JoinLocalPath(ExpandTilde(destination),
                    RemoteFileName(sources[n]));
            }
            else
            {
                localPath = ExpandTilde(destination);
            }
            unsigned long long size = 0;
            Console::Instance().TransferStart(RemoteFileName(sources[n]), 0);
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
    if (args.size() < 2)
    {
        Console::Instance().Error(Texts::WrongArguments("put",
            "Syntax: put <localfile(s)> <remotepath>"));
        mErrors++;
        return false;
    }
    std::string destination = ResolveRemotePath(mRemoteDirectory, args[args.size() - 1]);
    LscpAttrs destinationAttrs;
    bool destinationIsDir = (lscp_sftp_stat(destination.c_str(), &destinationAttrs) == 0) &&
        (destinationAttrs.flags & LSCP_ATTR_PERMISSIONS) &&
        ((destinationAttrs.permissions & 0040000) != 0);

    bool any = false;
    for (size_t i = 0; i + 1 < args.size(); i++)
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
        if (!ExpandLocalWildcard(ResolveLocalPath(current, args[i]), sources, error))
        {
            Console::Instance().Error(Texts::LocalError("expansion of", args[i], error));
            mErrors++;
            continue;
        }
        if (sources.empty())
        {
            Console::Instance().Error(Texts::LocalError("expansion of", args[i],
                "No such file."));
            mErrors++;
            continue;
        }
        for (size_t n = 0; n < sources.size(); n++)
        {
            LocalFileInfo info = StatLocal(sources[n]);
            if (info.isDir)
            {
                Console::Instance().Error(Texts::LocalError("upload of", sources[n],
                    "Directories are not supported yet; transfer the files inside."));
                mErrors++;
                continue;
            }
            std::string remotePath = (destinationIsDir || sources.size() > 1 ||
                args.size() > 2) ?
                JoinRemotePath(destination, LocalFileName(sources[n])) : destination;
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
        }
    }
    return any;
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
        for (size_t n = 0; n < paths.size(); n++)
        {
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
    long permissions = strtol(args[0].c_str(), NULL, 8);
    if (permissions <= 0 || permissions > 07777)
    {
        Console::Instance().Error(Texts::WrongArguments("chmod",
            "Permissions must be an octal number, for example 644."));
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
    if (args.size() != 2)
    {
        Console::Instance().Error(Texts::WrongArguments("option",
            "Syntax: option <name> <value> (supported: confirm)"));
        mErrors++;
        return false;
    }
    std::string name = ToLower(args[0]);
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

    char buffer[8192];
    while (!mExit && fgets(buffer, sizeof(buffer), file) != NULL)
    {
        ExecuteLine(buffer);
    }
    if (closeFile)
    {
        fclose(file);
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
