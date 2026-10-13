/*
 * Script: the command interpreter of the POSIX front end, following the
 * command set of the WinSCP console client so that existing scripts keep
 * working.
 */

#ifndef LINUXSCP_SCRIPT_H
#define LINUXSCP_SCRIPT_H

#include <string>
#include <vector>

#include "Editor.h"
#include "LocalFs.h"
#include "Session.h"
#include "SessionStore.h"
#include "SftpClient.h"
#include "SyncPlanner.h"

namespace linuxscp {

struct TransferFlags
{
    /* 'get -delete' / 'put -delete': remove the source after a transfer. */
    bool deleteSource;
    /* 'synchronize -delete': remove target entries that the source does
     * not have. It never removes the source, where the user's data
     * lives. */
    bool deleteObsolete;
    bool newerOnly;
    bool preserveTime;
    bool recursive;
    bool resume;

    TransferFlags() : deleteSource(false), deleteObsolete(false),
        newerOnly(false), preserveTime(false), recursive(true),
        resume(false) {}
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
    /* Reads one command per line from stdin and prints the outcome of
     * each one after a 'LSCP-RESULT' marker. This is how the graphical
     * client keeps a session open between two operations instead of
     * connecting again for every listing. */
    int RunServer();
    /* Executes one command line; returns false when the client must stop. */
    bool ExecuteLine(const std::string & line);

    int ErrorCount() const { return mErrors; }
    bool ExitRequested() const { return mExit; }
    /* True when the command that ran last reported an error. Used by the
     * server loop, which reports the outcome of every command. */
    bool LastCommandFailed() const { return mLastCommandFailed; }
    /* The stored sessions (site manager) of this client. */
    SessionStore & Sessions() { return mSessions; }
    /* The remote file listing as the synchronization planner needs it. */
    bool ListRemoteEntries(const std::string & path, std::vector<SyncEntry> & entries,
        std::string & error);
    /* The three remote operations the editor needs, so that Editor.cpp
     * does not have to know anything about the session. */
    int StatForEdit(const std::string & path, LscpAttrs & attrs, std::string & error);
    int DownloadForEdit(const std::string & remote, const std::string & local,
        bool preserveTime, std::string & error);
    int UploadForEdit(const std::string & local, const std::string & remote,
        bool preserveTime, std::string & error);


private:
    Script(const Script &);
    Script & operator=(const Script &);

    bool ExecuteCommand(const std::string & line);
    bool ValidateTransferSwitches(const std::string & command,
        const std::vector<std::string> & args);
    bool ListLocal(const std::vector<std::string> & args);
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
    /* Reads the switches of 'synchronize' and 'keepuptodate'. Returns
     * false with 'error' set for anything it does not support, so that
     * an unsupported switch is never silently ignored. */
    bool ParseSyncArguments(const std::vector<std::string> & args,
        const std::string & command, SyncOptions & options,
        std::vector<std::string> & dirs, bool & preview, int & interval,
        int & count, std::string & error);
    /* Compares the two trees and fills 'plan'. */
    bool BuildSynchronizationPlan(SyncOptions & options,
        const std::string & localDir, const std::string & remoteDir,
        SyncPlan & plan, SyncState * state, std::string & error);
    /* Applies a plan. Nothing is removed unless the plan was confirmed;
     * a plan with conflicts is applied only as far as the transfers. */
    bool ExecuteSynchronizationPlan(SyncPlan & plan, const SyncOptions & options,
        bool allowDelete, unsigned long long & bytesTransferred);
    /* Prints a plan in a fixed, tab-separated format that a script or
     * the graphical client can read back. */
    void PrintSynchronizationPlan(const SyncPlan & plan) const;
    bool KeepUpToDate(const std::vector<std::string> & args);
    bool EditRemoteFile(const std::vector<std::string> & args);
    bool SessionCommand(const std::vector<std::string> & args);
    bool SaveStoredSession(const std::vector<std::string> & args);
    bool RemoveStoredSessionEntry(const std::vector<std::string> & args);
    /* Opens a stored session by name; false when there is no such
     * session, in which case the caller falls back to a URL. */
    bool OpenStoredSite(const std::string & name);
    bool RemoveRemote(const std::vector<std::string> & args);
    bool RemoveRemoteDirectory(const std::vector<std::string> & args);
    bool MakeRemoteDirectory(const std::vector<std::string> & args);
    bool MoveRemote(const std::vector<std::string> & args);
    bool ChangeRemotePermissions(const std::vector<std::string> & args);
    bool StatRemote(const std::vector<std::string> & args);
    bool SetOption(const std::vector<std::string> & args);
    void OpenTerminal(const std::vector<std::string> & args);
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

    /* Records the timestamps and the size of a file that is (or has just
     * been brought) in sync, so that the next pass can tell a change on
     * one side from a change on both. */
    void RecordSyncState(SyncState * state, const std::string & relative,
        const std::string & localPath, const std::string & remotePath);

    /* Removes one entry, a directory tree included. Used by 'synchronize
     * -delete' for the entries the other side does not have. */
    bool RemoveLocalItemRecursive(const std::string & path);
    bool RemoveRemoteItemRecursive(const std::string & path);
    /* Asks before removing one obsolete entry; true when it may go. */
    bool ConfirmObsolete(const std::string & side, const std::string & path,
        bool isDirectory);

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
        unsigned long long resumeOffset, bool preserveTime, unsigned long long * size,
        LscpProgressFn progress, void * ctx, std::string & error);
    int RemotePut(const std::string & local, const std::string & remote,
        int permissions, unsigned long long resumeOffset, bool preserveTime,
        unsigned long long * size, LscpProgressFn progress, void * ctx,
        std::string & error);
    int RemoteRemove(const std::string & path, std::string & error);
    int RemoteRemoveDir(const std::string & path, std::string & error);
    int RemoteMakeDir(const std::string & path, int permissions, std::string & error);
    int RemoteRename(const std::string & from, const std::string & to, std::string & error);
    int RemoteSetPermissions(const std::string & path, int permissions, std::string & error);


    SessionOptions mDefaults;
    Session mSession;
    SessionStore mSessions;
    std::string mRemoteDirectory;
    /* Name of the stored session that was opened, so that 'savesession'
     * keeps it and 'open' can report it. */
    std::string mStoredSessionName;
    enum class BatchMode { Off, On, Abort, Continue };
    BatchMode mBatchMode;
    bool mBatchConfigured;
    bool mConfirm;
    bool mResume;
    bool mExit;
    int mErrors;
    bool mLastCommandFailed;
    /* 'option editor': the program 'edit' runs, or empty to use the
     * environment. */
    std::string mEditor;
    unsigned long long mEditMaxSize;
};

} // namespace linuxscp

#endif /* LINUXSCP_SCRIPT_H */
