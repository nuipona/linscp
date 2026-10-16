/*
 * ScpShell: remote shell filesystem engine for scp:// sessions.
 *
 * See ScpShell.h for the model.  Every operation opens a one-shot SSH
 * connection, runs a command and parses its output, which is exactly
 * how WinSCP's SCP backend works.  The main SFTP session stays open
 * while these run.
 */

#include "ScpShell.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "Platform.h"

namespace linuxscp {

bool ScpShell::Handles(const std::string & protocol)
{
    return protocol == "scp";
}

std::string ScpShell::ShellQuote(const std::string & path)
{
    /* POSIX safe-quoting: wrap in single quotes and escape any embedded
     * single quotes.  This handles spaces, newlines, $, `, !, #, etc. */
    std::string result = "'";
    for (size_t i = 0; i < path.size(); i++)
    {
        if (path[i] == '\'')
        {
            result += "'\\''";
        }
        else
        {
            result += path[i];
        }
    }
    result += "'";
    return result;
}

namespace {

/* Run a command over SSH exec and capture its output.  Returns the
 * command's exit code (0 = success, -1 = transport failure). */
int RunSshCommand(const SessionOptions & options,
    const std::string & command, std::string & output,
    std::string & error)
{
    output.clear();
    std::string captured;
    bool ok = Session::ExecRemote(options, command, error);
    if (!ok)
    {
        return -1;
    }
    /* ExecRemote prints output to the console; for parsed results we
     * need to capture stdout instead.  Use a one-shot curl-backed SSH
     * exec through the protocol client when available, falling back to
     * the Session's exec. */
    /* Build a minimal SSH exec command with output capture. */
    std::vector<std::string> args;
    std::string sshBin = FindExecutableOnPath("ssh");
    if (sshBin.empty())
    {
        sshBin = "ssh";
    }
    args.push_back(sshBin);
    args.push_back("-o");
    args.push_back("BatchMode=yes");
    args.push_back("-o");
    args.push_back("StrictHostKeyChecking=accept-new");
    if (options.portNumber > 0 && options.portNumber != 22)
    {
        args.push_back("-p");
        args.push_back(std::to_string(options.portNumber));
    }
    if (!options.privateKeyFile.empty())
    {
        args.push_back("-i");
        args.push_back(ExpandTilde(options.privateKeyFile));
    }
    if (!options.hostKey.empty() && options.hostKey == "*")
    {
        args.push_back("-o");
        args.push_back("StrictHostKeyChecking=no");
    }
    /* Build the user@host part. */
    std::string target;
    if (!options.userName.empty())
    {
        target = options.userName + "@";
    }
    target += options.hostName;
    args.push_back(target);
    args.push_back("--");
    args.push_back(command);

    std::string outStr;
    std::string errStr;
    int rc = RunCapture(args, outStr, errStr);
    if (rc == -1)
    {
        error = "Failed to run ssh. Install openssh-client and make sure "
            "it is on PATH.";
        return -1;
    }
    output = outStr;
    if (rc != 0 && errStr.empty())
    {
        errStr = "Remote command exited with code " + std::to_string(rc);
    }
    if (rc != 0)
    {
        error = errStr;
    }
    return rc;
}

/* Parse 'stat -c' output into LscpAttrs.  The format is:
 *   Access: (0755/drwxr-xr-x)  Uid: ( 1000/  user)  Gid: ( 1000/  group)
 *   Modify: 2023-06-25 11:22:33.000000000 +0000
 *   Size: 12345
 * We also support 'ls -l' style single-line output as a fallback. */
bool ParseStatOutput(const std::string & output, LscpAttrs * attrs)
{
    if (attrs == NULL)
    {
        return false;
    }
    attrs->flags = 0;

    /* Try 'ls -l' style first (single line). */
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        ListingEntry entry;
        if (ParseUnixListingLine(line, entry))
        {
            if (entry.size > 0 || !entry.isDir)
            {
                attrs->flags |= LSCP_ATTR_SIZE;
                attrs->size = entry.size;
            }
            if (entry.timeKnown)
            {
                attrs->flags |= LSCP_ATTR_ACMODTIME;
                attrs->mtime = entry.mtime;
                attrs->atime = entry.mtime;
            }
            attrs->flags |= LSCP_ATTR_PERMISSIONS;
            attrs->permissions = entry.permissions;
            return true;
        }
    }
    return false;
}

} // namespace

bool ScpShell::List(const SessionOptions & options,
    const std::string & remotePath,
    std::vector<ListingEntry> & entries, std::string & error)
{
    entries.clear();
    /* Use 'ls -la' which shows hidden files and the full listing.
     * The -1 flag ensures one entry per line. */
    const std::string cmd = "ls -la1 " + ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    if (rc != 0)
    {
        return false;
    }

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line))
    {
        /* Strip carriage returns. */
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        ListingEntry entry;
        if (ParseUnixListingLine(line, entry))
        {
            /* Skip . and .. */
            if (entry.name == "." || entry.name == "..")
            {
                continue;
            }
            entries.push_back(entry);
        }
        /* Lines that don't parse (total: header) are silently skipped. */
    }
    return true;
}

int ScpShell::Stat(const SessionOptions & options,
    const std::string & remotePath, LscpAttrs * attrs,
    std::string & error)
{
    /* Use 'stat -c' for structured output, falling back to 'ls -ld'. */
    const std::string cmd = "stat -c '%A %s %Y' " + ShellQuote(remotePath) +
        " 2>/dev/null || ls -ld " + ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    if (rc != 0)
    {
        if (output.find("No such file") != std::string::npos ||
            output.find("cannot access") != std::string::npos ||
            error.find("No such file") != std::string::npos)
        {
            return LSCP_SFTP_STAT_NOTEXISTS;
        }
        return LSCP_SFTP_STAT_ERROR;
    }

    /* Try parsing stat output: "0755 12345 1687693353" */
    std::istringstream stream(output);
    std::string permStr, sizeStr, mtimeStr;
    if (stream >> permStr >> sizeStr >> mtimeStr)
    {
        /* Parse octal permissions */
        if (!permStr.empty() && permStr[0] >= '0' && permStr[0] <= '7')
        {
            unsigned long perms = 0;
            for (size_t i = 0; i < permStr.size(); i++)
            {
                if (permStr[i] >= '0' && permStr[i] <= '7')
                {
                    perms = perms * 8 + (permStr[i] - '0');
                }
            }
            attrs->flags |= LSCP_ATTR_PERMISSIONS;
            attrs->permissions = perms;
        }

        /* Parse size */
        char * endPtr = NULL;
        unsigned long long sz = strtoull(sizeStr.c_str(), &endPtr, 10);
        if (endPtr != sizeStr.c_str())
        {
            attrs->flags |= LSCP_ATTR_SIZE;
            attrs->size = sz;
        }

        /* Parse mtime (epoch seconds) */
        unsigned long mt = strtoul(mtimeStr.c_str(), &endPtr, 10);
        if (endPtr != mtimeStr.c_str() && mt > 0)
        {
            attrs->flags |= LSCP_ATTR_ACMODTIME;
            attrs->mtime = mt;
            attrs->atime = mt;
        }

        return LSCP_SFTP_STAT_EXISTS;
    }

    /* Fallback: parse ls -l output. */
    ListingEntry entry;
    if (ParseUnixListingLine(output, entry))
    {
        attrs->flags = LSCP_ATTR_PERMISSIONS | LSCP_ATTR_SIZE;
        attrs->permissions = entry.permissions;
        attrs->size = entry.size;
        if (entry.timeKnown)
        {
            attrs->flags |= LSCP_ATTR_ACMODTIME;
            attrs->mtime = entry.mtime;
            attrs->atime = entry.mtime;
        }
        return LSCP_SFTP_STAT_EXISTS;
    }

    error = "Cannot parse stat output for '" + remotePath + "'.";
    return LSCP_SFTP_STAT_ERROR;
}

int ScpShell::MakeDir(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    const std::string cmd = "mkdir " + ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    return rc == 0 ? 0 : -1;
}

int ScpShell::Remove(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    const std::string cmd = "rm -- " + ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    return rc == 0 ? 0 : -1;
}

int ScpShell::RemoveDir(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    const std::string cmd = "rmdir -- " + ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    return rc == 0 ? 0 : -1;
}

int ScpShell::Move(const SessionOptions & options,
    const std::string & fromPath, const std::string & toPath,
    std::string & error)
{
    const std::string cmd = "mv -- " + ShellQuote(fromPath) + " " +
        ShellQuote(toPath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    return rc == 0 ? 0 : -1;
}

int ScpShell::SetPermissions(const SessionOptions & options,
    const std::string & remotePath, int permissions, std::string & error)
{
    char permBuf[16];
    snprintf(permBuf, sizeof(permBuf), "%04o", permissions & 07777);
    const std::string cmd = std::string("chmod ") + permBuf + " " +
        ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    return rc == 0 ? 0 : -1;
}

int ScpShell::SetTimes(const SessionOptions & options,
    const std::string & remotePath, unsigned long atime,
    unsigned long mtime, std::string & error)
{
    /* Use 'touch -t' with the format YYYYMMDDHHMMSS */
    struct tm tm;
    if (!LocalTime(static_cast<time_t>(mtime), tm))
    {
        error = "Cannot convert mtime for touch.";
        return -1;
    }
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%04d%02d%02d%02d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    const std::string cmd = std::string("touch -t ") + timeBuf + " " +
        ShellQuote(remotePath);
    std::string output;
    int rc = RunSshCommand(options, cmd, output, error);
    return rc == 0 ? 0 : -1;
}

bool ScpShell::Exec(const SessionOptions & options,
    const std::string & command, std::string & output,
    std::string & error)
{
    int rc = RunSshCommand(options, command, output, error);
    return rc == 0;
}

} // namespace linuxscp