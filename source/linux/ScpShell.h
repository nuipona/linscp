/*
 * ScpShell: WinSCP-style remote shell filesystem engine for SCP.
 *
 * The SCP protocol in WinSCP does not use the SFTP subsystem; instead
 * it runs commands over SSH exec ('ls -la', 'stat', 'chmod', etc.) and
 * parses the shell output.  This file implements that engine for LinSCP
 * so that scp:// sessions can list, stat, create and manage directories
 * and files through the remote shell, exactly like upstream.
 *
 * The engine opens one-shot SSH connections per operation (the same way
 * Session::ExecRemote works) and parses Unix 'ls -l' output.  Filenames
 * containing spaces, newlines and shell metacharacters are safely quoted
 * for the remote shell.
 *
 * This does NOT replace SFTP for sftp:// sessions; it fills the gap
 * for scp:// sessions that curl alone cannot serve with directory
 * operations.
 */

#ifndef LINUXSCP_SCPSHELL_H
#define LINUXSCP_SCPSHELL_H

#include <string>
#include <vector>

#include "ListingParse.h"
#include "Session.h"
#include "SftpClient.h"

namespace linuxscp {

class ScpShell
{
public:
    /* True when the protocol is served by this engine (scp only). */
    static bool Handles(const std::string & protocol);

    /* List a directory through 'ls -la' over SSH exec.  Parses the
     * remote output into ListingEntry records.  Falls back to 'ls -a'
     * when -la fails. */
    static bool List(const SessionOptions & options,
        const std::string & remotePath,
        std::vector<ListingEntry> & entries, std::string & error);

    /* Stat a single file through 'stat' over SSH exec.  Returns
     * LSCP_SFTP_STAT_* codes. */
    static int Stat(const SessionOptions & options,
        const std::string & remotePath, LscpAttrs * attrs,
        std::string & error);

    /* Create a directory through 'mkdir' over SSH exec. */
    static int MakeDir(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    /* Remove a file through 'rm' over SSH exec. */
    static int Remove(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    /* Remove an empty directory through 'rmdir' over SSH exec. */
    static int RemoveDir(const SessionOptions & options,
        const std::string & remotePath, std::string & error);

    /* Rename/move through 'mv' over SSH exec. */
    static int Move(const SessionOptions & options,
        const std::string & fromPath, const std::string & toPath,
        std::string & error);

    /* Change permissions through 'chmod' over SSH exec. */
    static int SetPermissions(const SessionOptions & options,
        const std::string & remotePath, int permissions,
        std::string & error);

    /* Set timestamps through 'touch -t' over SSH exec. */
    static int SetTimes(const SessionOptions & options,
        const std::string & remotePath, unsigned long atime,
        unsigned long mtime, std::string & error);

    /* Run an arbitrary command on the remote shell and return its
     * output.  Used by 'call' and '!' when the protocol is scp. */
    static bool Exec(const SessionOptions & options,
        const std::string & command, std::string & output,
        std::string & error);

private:
    /* Safely quote a filename for the remote POSIX shell. */
    static std::string ShellQuote(const std::string & path);
};

} // namespace linuxscp

#endif /* LINUXSCP_SCPSHELL_H */