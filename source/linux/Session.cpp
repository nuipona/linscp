#include "Session.h"

#include "Console.h"
#include "Texts.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "PuttyCompat.h"

namespace linuxscp {

namespace {

/* Deriving from the core's trait structs keeps static_cast working in both
 * directions, the way the Windows front end does it with __fastcall
 * classes. */
struct ScpSeat : public Seat
{
    Session * session;
};

struct ScpLogPolicy : public LogPolicy
{
    Session * session;
};

size_t seat_output(Seat * seat, SeatOutputType type, const void * data, size_t len)
{
    ScpSeat * scpSeat = static_cast<ScpSeat *>(seat);
    const char * bytes = static_cast<const char *>(data);

    /*
     * FORK_BUILD delivers the core's informational messages through the
     * same callback as the channel data, marked with a leading UTF-8 BOM
     * (FORK_BOM in putty.h). They are meant for the user and must never be
     * handed to the SFTP layer, so they are stripped off here.
     */
    const size_t bomLength = sizeof(FORK_BOM) - 1;
    if (len >= bomLength && memcmp(bytes, FORK_BOM, bomLength) == 0)
    {
        std::string message(bytes + bomLength, len - bomLength);
        while (!message.empty() &&
               (message[message.size() - 1] == '\n' ||
                message[message.size() - 1] == '\r'))
        {
            message.erase(message.size() - 1);
        }
        if (!message.empty())
        {
            Console::Instance().Status(message);
            Console::Instance().Status("\n");
        }
        return 0;
    }

    if (type == SEAT_OUTPUT_STDERR)
    {
        Console::Instance().Write(std::string(bytes, len));
        return 0;
    }
    scpSeat->session->PushReceivedData(bytes, len);
    return 0;
}

bool seat_eof(Seat * seat)
{
    return true;
}

void seat_notify_remote_exit(Seat * seat)
{
    Console::Instance().LogEvent("The remote side closed the connection.");
    static_cast<ScpSeat *>(seat)->session->NotifyRemoteExit();
}

void seat_notify_remote_disconnect(Seat * seat)
{
    Console::Instance().LogEvent("The remote side disconnected.");
    static_cast<ScpSeat *>(seat)->session->NotifyRemoteExit();
}

void seat_connection_fatal(Seat * seat, const char * message)
{
    Console::Instance().Fatal(message != NULL ? message : "Connection failed");
    Session * session = static_cast<ScpSeat *>(seat)->session;
    session->SetConnectionError(message != NULL ? message : "Connection failed");
    session->NotifyRemoteExit();
}

void seat_nonfatal(Seat * seat, const char * message)
{
    if (message != NULL)
    {
        Console::Instance().LogEvent(std::string("Warning: ") + message);
    }
}

SeatPromptResult seat_get_userpass_input(Seat * seat, prompts_t * prompts)
{
    Session * session = static_cast<ScpSeat *>(seat)->session;
    const SessionOptions & options = session->Options();
    Console & console = Console::Instance();

    if (prompts->instruction != NULL && prompts->instruction[0] != '\0')
    {
        console.WriteLine(prompts->instruction);
    }

    for (size_t i = 0; i < prompts->n_prompts; i++)
    {
        prompt_t * prompt = prompts->prompts[i];
        std::string text = prompt->prompt != NULL ? prompt->prompt : "";
        std::string answer;

        bool isUserName = prompt->echo &&
            (text.find("username") != std::string::npos ||
             text.find("sername") != std::string::npos ||
             text.find("login") != std::string::npos ||
             text.find("Login") != std::string::npos);
        bool isPassword = !prompt->echo &&
            (text.find("assword") != std::string::npos ||
             text.find("passphrase") != std::string::npos);

        if (isUserName && !options.userName.empty() && session->UserName().empty())
        {
            answer = options.userName;
        }
        else if (isPassword && !options.password.empty() && !session->PasswordUsed())
        {
            answer = options.password;
        }
        else if (console.IsInteractive())
        {
            if (!console.ReadLine(prompt->echo ? text : text, answer))
            {
                return make_spr_sw_abort_static("No more input available");
            }
            if (isUserName)
            {
                session->SetUserName(answer);
            }
        }
        else
        {
            return make_spr_sw_abort_static(
                "No stored answer for this prompt and not running interactively");
        }

        prompt_set_result(prompt, answer.c_str());
        if (isPassword)
        {
            session->MarkPasswordUsed();
        }
    }

    return SPR_OK;
}

SeatPromptResult seat_confirm_ssh_host_key(Seat * seat, const char * host, int port,
    const char * keytype, char * keystr, SeatDialogText * text, HelpCtx helpctx,
    void (*callback)(void * ctx, SeatPromptResult result), void * ctx,
    char ** fingerprints, bool is_certificate, int ca_count, bool already_verified)
{
    Session * session = static_cast<ScpSeat *>(seat)->session;
    Console & console = Console::Instance();
    /*
     * 'fingerprints' is indexed by FingerprintType, not NUL-terminated, and
     * each entry reads '<key type> <bits> <fingerprint>'. SHA-256 is what
     * everybody compares by these days, so that is the one to show.
     */
    std::string fingerprint;
    if (fingerprints != NULL && fingerprints[SSH_FPTYPE_SHA256] != NULL)
    {
        fingerprint = fingerprints[SSH_FPTYPE_SHA256];
    }
    else
    {
        for (int i = 0; fingerprints != NULL && i < SSH_N_FPTYPES; i++)
        {
            if (fingerprints[i] != NULL)
            {
                fingerprint = fingerprints[i];
                break;
            }
        }
    }
    if (fingerprint.empty())
    {
        fingerprint = "(no fingerprint available)";
    }

    if (already_verified)
    {
        return SPR_OK;
    }

    if (session->AcceptAnyHostKey())
    {
        store_host_key(seat, host, port, keytype, keystr);
        return SPR_OK;
    }

    if (session->ExpectHostKey())
    {
        if (session->HostKeyMatches(fingerprints))
        {
            store_host_key(seat, host, port, keytype, keystr);
            return SPR_OK;
        }
        console.Error(Texts::HostKeyUnexpected(host, port, keytype, fingerprint,
            session->ExpectedHostKey()));
        return make_spr_sw_abort_static("The host key does not match the expected one");
    }

    if (have_ssh_host_key(seat, host, port, keytype))
    {
        console.Error(Texts::HostKeyMismatch(host, port, keytype, fingerprint));
        return make_spr_sw_abort_static("The host key does not match the cached one");
    }

    console.Error(Texts::HostKeyUnknown(host, port, keytype, fingerprint));
    if (!console.IsInteractive())
    {
        return make_spr_sw_abort_static(
            "The host key is not cached; use -hostkey to accept it");
    }
    if (console.Confirm("Continue connecting and cache the host key?", false))
    {
        store_host_key(seat, host, port, keytype, keystr);
        return SPR_OK;
    }
    return SPR_USER_ABORT;
}

SeatPromptResult seat_confirm_weak_crypto_primitive(Seat * seat, SeatDialogText * text,
    void (*callback)(void * ctx, SeatPromptResult result), void * ctx,
    const char * algtype, const char * algname, int wcr)
{
    Console::Instance().Error(std::string("Warning: the server only offers a weak ") +
        algtype + " algorithm (" + algname + "). Continuing.");
    return SPR_OK;
}

SeatPromptResult seat_confirm_weak_cached_hostkey(Seat * seat, SeatDialogText * text,
    void (*callback)(void * ctx, SeatPromptResult result), void * ctx)
{
    Console::Instance().Error(
        "Warning: the cached host key uses a weak algorithm. Continuing.");
    return SPR_OK;
}

const char * seat_get_display(Seat * seat, SeatDisplayType dtype)
{
    return getenv("DISPLAY");
}

bool seat_is_utf8(Seat * seat)
{
    return true;
}

bool seat_verbose(Seat * seat)
{
    return static_cast<ScpSeat *>(seat)->session->Options().verbose;
}

bool seat_interactive(Seat * seat)
{
    return Console::Instance().IsInteractive();
}

/* One iteration of the event loop, so that the caller stays in control. */
bool pump_once(void * ctx, bool fd_ready, bool callback_ran)
{
    return false;
}

void log_policy_eventlog(LogPolicy * policy, const char * event)
{
    if (event != NULL)
    {
        Console::Instance().LogEvent(event);
    }
}

int log_policy_askappend(LogPolicy * policy, Filename * filename,
    void (*callback)(void * ctx, int result), void * ctx)
{
    return 2; /* overwrite */
}

void log_policy_logging_error(LogPolicy * policy, const char * event)
{
    Console::Instance().Error(std::string("Session log error: ") +
        (event != NULL ? event : "(unknown)"));
}

SeatVtable MakeSeatVtable()
{
    SeatVtable vt;
    memset(&vt, 0, sizeof(vt));
    vt.output = seat_output;
    vt.eof = seat_eof;
    vt.sent = nullseat_sent;
    vt.banner = nullseat_banner;
    vt.get_userpass_input = seat_get_userpass_input;
    vt.notify_session_started = nullseat_notify_session_started;
    vt.notify_remote_exit = seat_notify_remote_exit;
    vt.notify_remote_disconnect = seat_notify_remote_disconnect;
    vt.connection_fatal = seat_connection_fatal;
    vt.nonfatal = seat_nonfatal;
    vt.update_specials_menu = nullseat_update_specials_menu;
    vt.get_ttymode = nullseat_get_ttymode;
    vt.set_busy_status = nullseat_set_busy_status;
    vt.confirm_ssh_host_key = seat_confirm_ssh_host_key;
    vt.confirm_weak_crypto_primitive = seat_confirm_weak_crypto_primitive;
    vt.confirm_weak_cached_hostkey = seat_confirm_weak_cached_hostkey;
    vt.prompt_descriptions = nullseat_prompt_descriptions;
    vt.is_utf8 = seat_is_utf8;
    vt.echoedit_update = nullseat_echoedit_update;
    vt.get_display = seat_get_display;
    vt.get_windowid = nullseat_get_windowid;
    vt.get_window_pixel_size = nullseat_get_window_pixel_size;
    vt.stripctrl_new = nullseat_stripctrl_new;
    vt.set_trust_status = nullseat_set_trust_status;
    vt.can_set_trust_status = nullseat_can_set_trust_status_no;
    vt.has_mixed_input_stream = nullseat_has_mixed_input_stream_no;
    vt.verbose = seat_verbose;
    vt.interactive = seat_interactive;
    vt.get_cursor_position = nullseat_get_cursor_position;
    return vt;
}

LogPolicyVtable MakeLogPolicyVtable()
{
    LogPolicyVtable vt;
    memset(&vt, 0, sizeof(vt));
    vt.eventlog = log_policy_eventlog;
    vt.askappend = log_policy_askappend;
    vt.logging_error = log_policy_logging_error;
    vt.verbose = null_lp_verbose_no;
    return vt;
}

const SeatVtable seat_vtable = MakeSeatVtable();
const LogPolicyVtable log_policy_vtable = MakeLogPolicyVtable();

Session * g_currentSession = NULL;

} // namespace

SessionOptions::SessionOptions() :
    portNumber(22),
    timeoutSecs(15),
    verbose(false)
{
}

bool ParseSiteUrl(const std::string & url, SessionOptions & options, std::string & error)
{
    std::string rest = url;
    std::string scheme;
    std::string::size_type pos = rest.find("://");
    if (pos != std::string::npos)
    {
        scheme = rest.substr(0, pos);
        rest = rest.substr(pos + 3);
    }
    else
    {
        scheme = "sftp";
    }
    for (size_t i = 0; i < scheme.size(); i++)
    {
        scheme[i] = static_cast<char>(tolower(scheme[i]));
    }
    if (scheme != "sftp" && scheme != "ssh" && scheme != "scp")
    {
        error = "Unsupported protocol '" + scheme +
            "'. The Debian port currently supports SFTP only.";
        return false;
    }

    /* Split off the path. */
    std::string path;
    pos = rest.find('/');
    if (pos != std::string::npos)
    {
        path = rest.substr(pos);
        rest = rest.substr(0, pos);
    }

    /* Split off the credentials. */
    std::string auth;
    pos = rest.rfind('@');
    if (pos != std::string::npos)
    {
        auth = rest.substr(0, pos);
        rest = rest.substr(pos + 1);
    }
    if (!auth.empty())
    {
        pos = auth.find(':');
        if (pos != std::string::npos)
        {
            options.userName = auth.substr(0, pos);
            options.password = auth.substr(pos + 1);
        }
        else
        {
            options.userName = auth;
        }
    }

    /* Host and port. IPv6 literals are written as [::1] or [::1]:22. */
    if (!rest.empty() && rest[0] == '[')
    {
        std::string::size_type close = rest.find(']');
        if (close == std::string::npos)
        {
            error = "Malformed IPv6 address in '" + url + "'.";
            return false;
        }
        options.hostName = rest.substr(1, close - 1);
        std::string tail = rest.substr(close + 1);
        if (!tail.empty() && tail[0] == ':')
        {
            options.portNumber = atoi(tail.c_str() + 1);
        }
    }
    else
    {
        pos = rest.rfind(':');
        if (pos != std::string::npos)
        {
            options.hostName = rest.substr(0, pos);
            options.portNumber = atoi(rest.c_str() + pos + 1);
        }
        else
        {
            options.hostName = rest;
        }
    }

    if (options.hostName.empty())
    {
        error = "No host name in '" + url + "'.";
        return false;
    }
    if (options.portNumber <= 0 || options.portNumber > 65535)
    {
        error = "Invalid port number in '" + url + "'.";
        return false;
    }
    options.initialDirectory = path;
    return true;
}

Session::Session() :
    mSeat(NULL),
    mLogPolicy(NULL),
    mLogCtx(NULL),
    mBackend(NULL),
    mOpen(false),
    mAlive(false),
    mPasswordUsed(false),
    mReceivedStart(0),
    mReceivedEnd(0)
{
}

Session::~Session()
{
    Close();
}

std::string Session::UserName() const
{
    return mUserName;
}

std::string Session::HostDescription() const
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", mOptions.portNumber);
    return mOptions.hostName + ":" + buffer;
}

void Session::PushReceivedData(const void * data, size_t len)
{
    if (len == 0)
    {
        return;
    }
    const char * bytes = static_cast<const char *>(data);
    if (getenv("LSCP_TRACE") != NULL)
    {
        std::string dump = "SFTP rx ";
        char hex[8];
        for (size_t i = 0; i < len && i < 32; i++)
        {
            snprintf(hex, sizeof(hex), "%02X ",
                static_cast<unsigned char>(bytes[i]));
            dump += hex;
        }
        Console::Instance().LogEvent(dump);
    }
    Console::Instance().LogTraffic(data, len, true);
    /* Compact the buffer if the consumer has caught up. */
    if (mReceivedStart == mReceivedEnd)
    {
        mReceived.clear();
        mReceivedStart = 0;
        mReceivedEnd = 0;
    }
    mReceived.insert(mReceived.end(), bytes, bytes + len);
    mReceivedEnd = mReceived.size();
}

bool Session::TakeReceivedData(char * buffer, size_t len)
{
    if (mReceivedEnd - mReceivedStart < len)
    {
        return false;
    }
    memcpy(buffer, &mReceived[mReceivedStart], len);
    mReceivedStart += len;
    if (mReceivedStart == mReceivedEnd)
    {
        mReceived.clear();
        mReceivedStart = 0;
        mReceivedEnd = 0;
    }
    return true;
}

bool Session::SendData(const char * data, size_t len)
{
    if (mBackend == NULL)
    {
        return false;
    }
    Console::Instance().LogTraffic(data, len, false);
    backend_send(static_cast<Backend *>(mBackend), data, len);
    return true;
}

size_t Session::SendBufferSize()
{
    if (mBackend == NULL)
    {
        return 0;
    }
    return backend_sendbuffer(static_cast<Backend *>(mBackend));
}

bool Session::Pump()
{
    if (mBackend == NULL)
    {
        return false;
    }
    cli_main_loop(cliloop_no_pw_setup, cliloop_no_pw_check, pump_once, this);
    return mAlive;
}

bool Session::ReceiveData(char * buffer, size_t len)
{
    while (!TakeReceivedData(buffer, len))
    {
        if (!mAlive)
        {
            Console::Instance().LogEvent(std::string("The session is no longer alive: ") +
                (mConnectionError.empty() ? std::string("closed by the server") : mConnectionError));
            return false;
        }
        if (!Pump())
        {
            return false;
        }
    }
    return true;
}

void Session::NotifyRemoteExit()
{
    mAlive = false;
}

void Session::SetConnectionError(const std::string & error)
{
    mConnectionError = error;
}

bool Session::HostKeyMatches(const char * const * fingerprints) const
{
    const std::string expected = mOptions.hostKey;
    if (expected.empty())
    {
        return false;
    }
    for (int i = 0; fingerprints != NULL && i < SSH_N_FPTYPES; i++)
    {
        if (fingerprints[i] == NULL)
        {
            continue;
        }
        /*
         * The core hands the fingerprint over as '<key type> <bits>
         * <fingerprint>', e.g. 'ssh-rsa 2048 SHA256:xyz'. Accept a match
         * against the whole string, against the fingerprint on its own, or
         * against either without the 'SHA256:'/'MD5:' marker in front.
         */
        const std::string candidate = fingerprints[i];
        const std::string::size_type lastSpace = candidate.find_last_of(" \t");
        const std::string tail = candidate.substr(lastSpace + 1);
        const std::string forms[2] = { candidate, tail };
        for (int n = 0; n < 2; n++)
        {
            const std::string & form = forms[n];
            if (form == expected)
            {
                return true;
            }
            const std::string::size_type colon = form.find(':');
            if (colon == std::string::npos)
            {
                continue;
            }
            const std::string prefix = form.substr(0, colon);
            if ((prefix == "SHA256" || prefix == "SHA-256" || prefix == "MD5") &&
                form.substr(colon + 1) == expected)
            {
                return true;
            }
        }
    }
    return false;
}

bool Session::Open(const SessionOptions & options, std::string & error)
{
    Close();

    mOptions = options;
    mUserName = options.userName;
    mPasswordUsed = false;
    mAlive = true;
    mConnectionError.clear();

    ScpSeat * seat = new ScpSeat();
    seat->session = this;
    seat->vt = &seat_vtable;
    mSeat = seat;

    ScpLogPolicy * logPolicy = new ScpLogPolicy();
    logPolicy->session = this;
    logPolicy->vt = &log_policy_vtable;
    mLogPolicy = logPolicy;

    Console::Instance().LogEvent("Preparing the session configuration");
    Conf * conf = conf_new();
    do_defaults(NULL, conf);
    Console::Instance().LogEvent("Loaded the default configuration");
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    conf_set_str(conf, CONF_host, mOptions.hostName.c_str());
    conf_set_int(conf, CONF_port, mOptions.portNumber);
    if (!mOptions.userName.empty())
    {
        conf_set_str(conf, CONF_username, mOptions.userName.c_str());
    }
    if (!mOptions.privateKeyFile.empty())
    {
        Filename * fileName = filename_from_str(mOptions.privateKeyFile.c_str());
        conf_set_filename(conf, CONF_keyfile, fileName);
        filename_free(fileName);
    }
    /* Run the SFTP subsystem, like psftp does. */
    conf_set_bool(conf, CONF_ssh_subsys, true);
    conf_set_str(conf, CONF_remote_cmd, "sftp");
    conf_set_bool(conf, CONF_ssh_simple, true);
    conf_set_bool(conf, CONF_nopty, true);
    conf_set_int(conf, CONF_connect_timeout, mOptions.timeoutSecs * 1000);
    conf_set_bool(conf, CONF_ssh_show_banner, true);
    conf_set_int(conf, CONF_proxy_log_to_term, FORCE_OFF);

    mLogCtx = log_init(static_cast<LogPolicy *>(mLogPolicy), conf);

    Console::Instance().LogEvent("Starting the SSH connection to " + HostDescription());
    char * realHost = NULL;
    Backend * backend = NULL;
    char * initError = backend_init(&ssh_backend, static_cast<Seat *>(mSeat),
        &backend, static_cast<LogContext *>(mLogCtx), conf,
        mOptions.hostName.c_str(), mOptions.portNumber, &realHost, true, false);
    conf_free(conf);
    mBackend = backend;

    if (initError != NULL)
    {
        error = initError;
        sfree(initError);
        Close();
        return false;
    }
    sfree(realHost);

    SetCurrentSession(this);

    Console::Instance().LogEvent("Waiting for the SSH channel to open");
    /* Wait until the SSH channel is ready to carry the SFTP protocol. */
    while (mAlive && !backend_sendok(static_cast<Backend *>(mBackend)))
    {
        Pump();
    }
    if (!mAlive)
    {
        error = mConnectionError.empty() ? Texts::ConnectionClosed() : mConnectionError;
        Close();
        return false;
    }

    mOpen = true;
    return true;
}

void Session::Close()
{
    if (g_currentSession == this)
    {
        SetCurrentSession(NULL);
    }
    if (mBackend != NULL)
    {
        backend_free(static_cast<Backend *>(mBackend));
        mBackend = NULL;
    }
    if (mLogCtx != NULL)
    {
        log_free(static_cast<LogContext *>(mLogCtx));
        mLogCtx = NULL;
    }
    if (mSeat != NULL)
    {
        delete static_cast<ScpSeat *>(mSeat);
        mSeat = NULL;
    }
    if (mLogPolicy != NULL)
    {
        delete static_cast<ScpLogPolicy *>(mLogPolicy);
        mLogPolicy = NULL;
    }
    mReceived.clear();
    mReceivedStart = 0;
    mReceivedEnd = 0;
    mOpen = false;
    mAlive = false;
}

Session * CurrentSession()
{
    return g_currentSession;
}

void SetCurrentSession(Session * session)
{
    g_currentSession = session;
}

} // namespace linuxscp
