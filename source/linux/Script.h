/*
 * Script: the command interpreter of the POSIX front end, following the
 * command set of the WinSCP console client so that existing scripts keep
 * working.
 */

#ifndef LINUXSCP_SCRIPT_H
#define LINUXSCP_SCRIPT_H

#include <string>
#include <vector>

#include "Session.h"
#include "SftpClient.h"

namespace linuxscp {

struct TransferFlags
{
    bool deleteSource;
    bool newerOnly;
    bool preserveTime;
    bool recursive;
    bool resume;

    TransferFlags() : deleteSource(false), newerOnly(false),
        preserveTime(false), recursive(true), resume(false) {}
};

class Script
{
public:
    Script();

    /* Options that 'open' fills in when the URL does not say otherwise. */
    void SetDefaults(const SessionOptions & options) { mDefaults = options; }

    /* Runs a whole script file ('-' for standard input). */
    int RunFile(const std::string & path);
    /* Runs the command(s) given on the command line, ';' separated. */
    int RunCommand(const std::string & line);
    /* Reads commands from the terminal until 'exit'. */
    int RunInteractive(const std::string & initialUrl);
    /* Executes one command line; returns false when the client must stop. */
    bool ExecuteLine(const std::string & line);

    int ErrorCount() const { return mErrors; }
    bool ExitRequested() const { return mExit; }

private:
    Script(const Script &);
    Script & operator=(const Script &);

    bool OpenSite(const std::vector<std::string> & args);
    void CloseSite();
    bool ChangeRemoteDirectory(const std::vector<std::string> & args);
    void PrintRemoteDirectory();
    bool ListRemote(const std::vector<std::string> & args);
    bool ChangeLocalDirectory(const std::vector<std::string> & args);
    void PrintLocalDirectory();
    bool GetFiles(const std::vector<std::string> & args);
    bool PutFiles(const std::vector<std::string> & args);
    bool SynchronizeDirectories(const std::vector<std::string> & args);
    bool RemoveRemote(const std::vector<std::string> & args);
    bool RemoveRemoteDirectory(const std::vector<std::string> & args);
    bool MakeRemoteDirectory(const std::vector<std::string> & args);
    bool MoveRemote(const std::vector<std::string> & args);
    bool ChangeRemotePermissions(const std::vector<std::string> & args);
    bool StatRemote(const std::vector<std::string> & args);
    bool SetOption(const std::vector<std::string> & args);
    void PrintHelp();
    void PrintStatus();

    bool DownloadDirectoryRecursive(const std::string & remoteDir,
        const std::string & localDir, const TransferFlags & flags,
        unsigned long long & bytesTransferred);
    bool UploadDirectoryRecursive(const std::string & localDir,
        const std::string & remoteDir, const TransferFlags & flags,
        unsigned long long & bytesTransferred);
    bool CreateRemoteDirectoryRecursive(const std::string & path,
        std::string & error);

    bool RequireConnection();
    /* Expands a remote argument (possibly a mask) into full paths. */
    bool ExpandRemote(const std::string & pattern, std::vector<std::string> & paths,
        std::string & error);
    void PrintRemoteEntry(const std::string & name, const LscpName & entry,
        bool longFormat = true);

    bool RemoteListDir(const std::string & path, LscpNames *& names, std::string & error);
    void RemoteFreeNames(LscpNames * names);
    int RemoteStat(const std::string & path, LscpAttrs * attrs, std::string & error);
    int RemoteGet(const std::string & remote, const std::string & local,
        unsigned long long resumeOffset, unsigned long long * size,
        LscpProgressFn progress, void * ctx, std::string & error);
    int RemotePut(const std::string & local, const std::string & remote,
        int permissions, unsigned long long resumeOffset, unsigned long long * size,
        LscpProgressFn progress, void * ctx, std::string & error);
    int RemoteRemove(const std::string & path, std::string & error);
    int RemoteRemoveDir(const std::string & path, std::string & error);
    int RemoteMakeDir(const std::string & path, int permissions, std::string & error);
    int RemoteRename(const std::string & from, const std::string & to, std::string & error);
    int RemoteSetPermissions(const std::string & path, int permissions, std::string & error);

    SessionOptions mDefaults;
    Session mSession;
    std::string mRemoteDirectory;
    bool mConfirm;
    bool mResume;
    bool mExit;
    int mErrors;
};

} // namespace linuxscp

#endif /* LINUXSCP_SCRIPT_H */
