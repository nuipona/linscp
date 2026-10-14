/*
 * Session: one SSH connection to an SFTP server, driving the shared
 * PuTTY core (source/putty) through its Seat and LogPolicy traits.
 *
 * The console client keeps exactly one session at a time; the SFTP
 * transport glue in PuttyGlue.cpp reaches it through CurrentSession().
 */

#ifndef LINUXSCP_SESSION_H
#define LINUXSCP_SESSION_H

#include <cstddef>
#include <string>
#include <vector>

#include "Platform.h"

namespace linuxscp {

/* True when 'protocol' is served by the native PuTTY SFTP engine.
 * Only SFTP qualifies, and only where that engine exists (POSIX);
 * on Windows SFTP is served by ProtocolClient (curl) instead and
 * this returns false for every protocol. All protocol dispatch in
 * Script goes through this helper, never through a bare comparison
 * against "sftp". */
inline bool UseNativeSftp(const std::string & protocol)
{
    return kHasNativeSftp && protocol == "sftp";
}

struct SessionOptions
{
    std::string protocol;
    std::string hostName;
    int portNumber;
    std::string userName;
    std::string password;
    std::string privateKeyFile;
    /* Expected host key: empty = use the cache/ask, "*" = accept any. */
    std::string hostKey;
    /* Directory to change to once connected (from the URL path). */
    std::string initialDirectory;
    int timeoutSecs;
    bool verbose;

    SessionOptions();
};

/* Parses 'sftp://[user[:password]@]host[:port]/[path]' (the password may
 * also be given by -password= or LINUXSCP_PASSWORD). */
bool ParseSiteUrl(const std::string & url, SessionOptions & options,
    std::string & error);

class Session
{
public:
    Session();
    ~Session();

    bool Open(const SessionOptions & options, std::string & error);
    void Close();
    bool IsOpen() const { return mOpen; }
    bool IsAlive() const { return mAlive; }

    const SessionOptions & Options() const { return mOptions; }
    /* struct Seat *, for get_log_seat(). */
    void * SeatHandle() const { return mSeat; }
    const std::string & ConnectionError() const { return mConnectionError; }
    std::string UserName() const;
    std::string HostDescription() const;

    /* Transport used by the SFTP layer. ReceiveData blocks (running the
     * event loop) until len bytes are available or the session dies. */
    bool SendData(const char * data, size_t len);
    size_t SendBufferSize();
    bool ReceiveData(char * buffer, size_t len);

    /* Called by the seat's output handler and by the main loop. */
    void PushReceivedData(const void * data, size_t len);
    bool Pump();

    /* Receives the channel data (stdout) of this session instead of
     * buffering it for the SFTP protocol layer. Used by exec sessions. */
    typedef void (*OutputSinkFn)(void * ctx, const char * data, size_t len);
    void SetOutputSink(OutputSinkFn sink, void * ctx);
    /* Where the seat hands channel data over: to the output sink of an
     * exec session, or into the SFTP receive buffer. */
    void DeliverChannelData(const void * data, size_t len);

    /* Runs 'command' on the server in a one-shot second SSH connection
     * (the 'exec' request instead of the SFTP subsystem), printing its
     * output as it arrives, and returns when the command ends. The
     * current session, if any, stays connected while this runs. */
    static bool ExecRemote(const SessionOptions & options,
        const std::string & command, std::string & error);

    /* Called by the seat. */
    void NotifyRemoteExit();
    void SetConnectionError(const std::string & error);
    void SetUserName(const std::string & userName) { mUserName = userName; }
    bool PasswordUsed() const { return mPasswordUsed; }
    void MarkPasswordUsed() { mPasswordUsed = true; }

    /* Host key policy, used by the seat's confirm_ssh_host_key. */
    bool HostKeyMatches(const char * const * fingerprints) const;
    bool AcceptAnyHostKey() const { return mOptions.hostKey == "*"; }
    bool ExpectHostKey() const { return !mOptions.hostKey.empty() && mOptions.hostKey != "*"; }
    const std::string & ExpectedHostKey() const { return mOptions.hostKey; }

private:
    Session(const Session &);
    Session & operator=(const Session &);

    bool TakeReceivedData(char * buffer, size_t len);

    SessionOptions mOptions;
    void * mSeat;              /* struct Seat * */
    void * mLogPolicy;         /* struct LogPolicy * */
    void * mLogCtx;            /* struct LogContext * */
    void * mBackend;           /* struct Backend * */
    bool mOpen;
    bool mAlive;
    std::string mConnectionError;
    std::string mUserName;
    bool mPasswordUsed;
    std::vector<char> mReceived;
    size_t mReceivedStart;
    size_t mReceivedEnd;
    OutputSinkFn mOutputSink;
    void * mOutputSinkCtx;

    /* Shared connection setup of Open() and ExecRemote(): connects and
     * starts 'program' (subsystem or exec), optionally becoming the
     * session the SFTP transport glue talks to. */
    bool Connect(const SessionOptions & options, const char * program,
        bool subsystem, bool takeCurrent, std::string & error);
};

/* The one session the console client is driving, or NULL. */
Session * CurrentSession();
void SetCurrentSession(Session * session);

} // namespace linuxscp

#endif /* LINUXSCP_SESSION_H */
