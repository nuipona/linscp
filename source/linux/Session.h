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

/* True when 'protocol' is served by the native PuTTY SFTP engine rather
 * than by the curl backend of ProtocolClient. All protocol dispatch in
 * Script goes through this helper, never through a bare comparison
 * against "sftp". */
inline bool UseNativeSftp(const std::string & protocol)
{
    return protocol == "sftp";
}

/*
 * The proxy of a session, under the names upstream's session settings use
 * ('None;SOCKS4;SOCKS5;HTTP;Telnet;Cmd'). 'Local' is upstream's pmCmd: a
 * command run locally whose standard input and output carry the connection.
 */
enum class ProxyMethod
{
    None,
    Socks4,
    Socks5,
    Http,
    Telnet,
    Local
};

/* Upstream's spelling of a method, as 'ProxyMethod=' accepts it. */
const char * ProxyMethodName(ProxyMethod method);
/* The port a method uses when the session does not name one. */
int ProxyDefaultPort(ProxyMethod method);
/* Accepts upstream's names ('socks5', 'Cmd') and its numbers ('0'..'5'). */
bool ParseProxyMethod(const std::string & text, ProxyMethod & method);

struct ProxySettings
{
    ProxyMethod method;
    std::string host;
    int port;               /* 0: use ProxyDefaultPort(method) */
    std::string userName;
    std::string password;
    /* 'ProxyTelnetCommand': the telnet command, or - for the 'Local'
     * method - the command to run. '%host' and '%port' are substituted. */
    std::string command;
    /* 'ProxyExcludeList': destinations reached without the proxy. */
    std::string excludeList;
    /* 'ProxyDNS': 0 = resolve here, 1 = automatic, 2 = let the proxy
     * resolve. Automatic follows the transport: everything but SOCKS4
     * leaves the name to the proxy. */
    int dns;
    /* 'ProxyLocalhost': send localhost through the proxy too. */
    bool evenLocalhost;

    ProxySettings() : method(ProxyMethod::None), port(0), dns(1),
        evenLocalhost(false) {}

    bool Active() const { return method != ProxyMethod::None; }
    /* The port actually used: the one given, or the method's default. */
    int EffectivePort() const
        { return port > 0 ? port : ProxyDefaultPort(method); }
    /* Whether the proxy resolves the destination name, which is what
     * decides between curl's 'socks5' and 'socks5h'. */
    bool ResolvesRemotely() const
        { return dns == 2 || (dns == 1 && method != ProxyMethod::Socks4); }
    bool operator==(const ProxySettings & other) const;
};

/*
 * One 'name=value' pair of a '-rawsettings' switch. The names are
 * upstream's session setting names; this build applies the proxy ones and
 * refuses the rest by name rather than accepting a setting it would drop.
 * Returns false with 'error' explaining the refusal.
 */
bool ApplyProxyRawSetting(const std::string & name, const std::string & value,
    ProxySettings & proxy, std::string & error);
/* The setting names '-rawsettings' accepts, for error messages and help. */
std::vector<std::string> ProxyRawSettingNames();
/* True when 'name' is a raw setting this build applies. */
bool IsProxyRawSettingName(const std::string & name);
/*
 * Checks a proxy configuration before a connection is made: a method that
 * needs a host has one, a port is in range, and the command of a telnet or
 * local proxy is there. Returns false with 'error' saying what is missing.
 */
bool ValidateProxySettings(const ProxySettings & proxy, std::string & error);
/*
 * Whether a curl-backed protocol can use this proxy. SOCKS and HTTP proxies
 * are curl options; a telnet or local command is not, and connecting
 * without the proxy the session asked for is worse than refusing.
 */
bool ProxyUsableByCurl(const ProxySettings & proxy, std::string & error);

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
    /* The proxy this session connects through, if any. */
    ProxySettings proxy;

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
