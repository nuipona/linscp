#include "Session.h"

#include "Console.h"
#include "PathUtil.h"
#include "ProtocolClient.h"
#include "SftpClient.h"
#include "Texts.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "PuttyCompat.h"

namespace linuxscp {

namespace {

/* Deriving from the core's trait structs keeps static_cast working in both
 * directions. */
struct ScpSeat : public Seat
{
    Session * session;
};

std::string LowerCaseAscii(const std::string & text)
{
    std::string result = text;
    for (size_t i = 0; i < result.size(); i++)
    {
        result[i] = static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

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
    scpSeat->session->DeliverChannelData(bytes, len);
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

        const std::string lowerPrompt = LowerCaseAscii(text);
        bool isUserName = prompt->echo &&
            (lowerPrompt.find("username") != std::string::npos ||
             lowerPrompt.find("user name") != std::string::npos ||
             lowerPrompt.find("login") != std::string::npos);
        bool isPassword = !prompt->echo &&
            (lowerPrompt.find("password") != std::string::npos ||
             lowerPrompt.find("passphrase") != std::string::npos);

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
            const bool read = prompt->echo ?
                console.ReadLine(text, answer) : console.ReadPassword(text, answer);
            if (!read)
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
    /* One machine readable line for whatever the server just offered, so
     * that a script or the graphical interface can pin this key later
     * instead of being told only that something went wrong. */
    const std::string hostkeyLine = "HOSTKEY " + std::string(keytype) + " " +
        fingerprint;

    /* An explicit pin must be checked even if the core can also establish
     * trust from its cache or a host certificate. */
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

    /* '*' is a deliberate one-session override, not a trust decision: do
     * not poison the persistent host-key cache with an unchecked key.
     * Report what was accepted anyway, so that a client (the graphical
     * interface, an automation script) can pin this key for the next
     * connection instead of accepting any key again. */
    if (session->AcceptAnyHostKey())
    {
        console.WriteLine(hostkeyLine);
        return SPR_OK;
    }

    if (already_verified)
    {
        return SPR_OK;
    }

    /*
     * Consult the host key cache. The core hands every connection to the
     * Seat, so this comparison is what makes a cached key trusted: a key
     * that matches the stored one is accepted without a prompt, a key
     * that differs from a stored one is refused, and only a host with no
     * cached key at all is a new host.
     */
    const int cacheState = linuxscp_check_cached_host_key(host, port, keytype, keystr);
    if (cacheState > 0)
    {
        Console::Instance().LogEvent(Texts::HostKeyCached(host, port));
        return SPR_OK;
    }
    if (cacheState < 0)
    {
        console.WriteLine(hostkeyLine);
        console.Error(Texts::HostKeyMismatch(host, port, keytype, fingerprint));
        return make_spr_sw_abort_static("The host key does not match the cached one");
    }

    console.WriteLine(hostkeyLine);
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
    protocol("sftp"),
    portNumber(22),
    timeoutSecs(15),
    verbose(false)
{
}

namespace {

std::string LowerAscii(const std::string & text)
{
    std::string result = text;
    for (size_t i = 0; i < result.size(); i++)
    {
        result[i] = static_cast<char>(
            tolower(static_cast<unsigned char>(result[i])));
    }
    return result;
}

std::string TrimAscii(const std::string & text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return std::string();
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

/* The methods by their upstream names, in upstream's numeric order. */
struct ProxyMethodEntry
{
    ProxyMethod method;
    const char * name;
    int defaultPort;
};

/* 'a, b and c', for the names a message offers. */
std::string JoinNames(const std::vector<std::string> & names)
{
    std::string result;
    for (size_t i = 0; i < names.size(); i++)
    {
        if (i > 0)
        {
            result += i + 1 == names.size() ? " and " : ", ";
        }
        result += names[i];
    }
    return result;
}

const ProxyMethodEntry kProxyMethods[] = {
    { ProxyMethod::None,   "None",   0 },
    { ProxyMethod::Socks4, "SOCKS4", 1080 },
    { ProxyMethod::Socks5, "SOCKS5", 1080 },
    { ProxyMethod::Http,   "HTTP",   80 },
    { ProxyMethod::Telnet, "Telnet", 23 },
    { ProxyMethod::Local,  "Cmd",    0 }
};

const size_t kProxyMethodCount =
    sizeof(kProxyMethods) / sizeof(kProxyMethods[0]);

/* 'ProxyDNS' as upstream stores it: 0 = off, 1 = automatic, 2 = on. */
bool ParseAutoSwitch(const std::string & text, int & value)
{
    const std::string lower = LowerAscii(TrimAscii(text));
    if (lower == "0" || lower == "off" || lower == "no" || lower == "false")
    {
        value = 0;
    }
    else if (lower == "1" || lower == "auto" || lower == "automatic")
    {
        value = 1;
    }
    else if (lower == "2" || lower == "on" || lower == "yes" || lower == "true")
    {
        value = 2;
    }
    else
    {
        return false;
    }
    return true;
}

bool ParseToggle(const std::string & text, bool & value)
{
    int parsed = 0;
    if (!ParseAutoSwitch(text, parsed))
    {
        return false;
    }
    value = parsed != 0;
    return true;
}

bool ParsePortValue(const std::string & text, int & port)
{
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty())
    {
        return false;
    }
    unsigned int value = 0;
    for (size_t i = 0; i < trimmed.size(); i++)
    {
        const unsigned char c = static_cast<unsigned char>(trimmed[i]);
        if (c < '0' || c > '9')
        {
            return false;
        }
        const unsigned int digit = c - '0';
        if (value > (65535U - digit) / 10U)
        {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value == 0 || value > 65535U)
    {
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

} // namespace

const char * ProxyMethodName(ProxyMethod method)
{
    for (size_t i = 0; i < kProxyMethodCount; i++)
    {
        if (kProxyMethods[i].method == method)
        {
            return kProxyMethods[i].name;
        }
    }
    return "None";
}

int ProxyDefaultPort(ProxyMethod method)
{
    for (size_t i = 0; i < kProxyMethodCount; i++)
    {
        if (kProxyMethods[i].method == method)
        {
            return kProxyMethods[i].defaultPort;
        }
    }
    return 0;
}

bool ParseProxyMethod(const std::string & text, ProxyMethod & method)
{
    const std::string lower = LowerAscii(TrimAscii(text));
    if (lower.empty())
    {
        return false;
    }
    for (size_t i = 0; i < kProxyMethodCount; i++)
    {
        if (lower == LowerAscii(kProxyMethods[i].name))
        {
            method = kProxyMethods[i].method;
            return true;
        }
    }
    /* Upstream also stores the method as its number, and 'local' is what
     * the local-command proxy is called outside PuTTY's own table. */
    if (lower == "local" || lower == "command")
    {
        method = ProxyMethod::Local;
        return true;
    }
    if (lower == "socks")
    {
        method = ProxyMethod::Socks5;
        return true;
    }
    /* Upstream stores the method as its number as well as its name, and
     * the numbers are the indices of the table above: 0 through 5. */
    if (lower.size() == 1 && lower[0] >= '0' &&
        static_cast<size_t>(lower[0] - '0') < kProxyMethodCount)
    {
        method = kProxyMethods[static_cast<size_t>(lower[0] - '0')].method;
        return true;
    }
    return false;
}

bool ProxySettings::operator==(const ProxySettings & other) const
{
    return method == other.method && host == other.host && port == other.port &&
        userName == other.userName && password == other.password &&
        command == other.command && excludeList == other.excludeList &&
        dns == other.dns && evenLocalhost == other.evenLocalhost;
}

std::vector<std::string> ProxyRawSettingNames()
{
    std::vector<std::string> names;
    names.push_back("ProxyMethod");
    names.push_back("ProxyHost");
    names.push_back("ProxyPort");
    names.push_back("ProxyUsername");
    names.push_back("ProxyPassword");
    names.push_back("ProxyTelnetCommand");
    names.push_back("ProxyLocalCommand");
    names.push_back("ProxyDNS");
    names.push_back("ProxyLocalhost");
    names.push_back("ProxyExcludeList");
    return names;
}

bool IsProxyRawSettingName(const std::string & name)
{
    const std::string lower = LowerAscii(TrimAscii(name));
    const std::vector<std::string> names = ProxyRawSettingNames();
    for (size_t i = 0; i < names.size(); i++)
    {
        if (lower == LowerAscii(names[i]))
        {
            return true;
        }
    }
    return false;
}

bool ApplyProxyRawSetting(const std::string & name, const std::string & value,
    ProxySettings & proxy, std::string & error)
{
    error.clear();
    const std::string lower = LowerAscii(TrimAscii(name));
    const std::string text = TrimAscii(value);

    if (lower == "proxymethod")
    {
        if (!ParseProxyMethod(text, proxy.method))
        {
            error = "Unknown proxy method '" + value + "' (use None, SOCKS4, "
                "SOCKS5, HTTP, Telnet or Cmd).";
            return false;
        }
        return true;
    }
    if (lower == "proxyhost")
    {
        proxy.host = text;
        return true;
    }
    if (lower == "proxyport")
    {
        int port = 0;
        if (!ParsePortValue(text, port))
        {
            error = "The proxy port '" + value +
                "' is not a number between 1 and 65535.";
            return false;
        }
        proxy.port = port;
        return true;
    }
    if (lower == "proxyusername")
    {
        proxy.userName = text;
        return true;
    }
    if (lower == "proxypassword")
    {
        /* Not trimmed beyond the ends: a password may hold spaces. */
        size_t start = value.find_first_not_of(" \t\r\n");
        size_t end = value.find_last_not_of(" \t\r\n");
        proxy.password = start == std::string::npos ? std::string() :
            value.substr(start, end - start + 1);
        return true;
    }
    if (lower == "proxytelnetcommand" || lower == "proxylocalcommand")
    {
        /* Upstream keeps one setting for both and reads it into the local
         * command when the method is 'Cmd'; the value is used verbatim, so
         * a trailing '\n' stays the two characters the user wrote. */
        proxy.command = value;
        return true;
    }
    if (lower == "proxydns")
    {
        int dns = 1;
        if (!ParseAutoSwitch(text, dns))
        {
            error = "The value of 'ProxyDNS' must be 0 (off), 1 (automatic) "
                "or 2 (on), not '" + value + "'.";
            return false;
        }
        proxy.dns = dns;
        return true;
    }
    if (lower == "proxylocalhost")
    {
        bool toggle = false;
        if (!ParseToggle(text, toggle))
        {
            error = "The value of 'ProxyLocalhost' must be 0 or 1, not '" +
                value + "'.";
            return false;
        }
        proxy.evenLocalhost = toggle;
        return true;
    }
    if (lower == "proxyexcludelist")
    {
        proxy.excludeList = text;
        return true;
    }

    error = "The session setting '" + name + "' is not one this build "
        "applies (it applies " + JoinNames(ProxyRawSettingNames()) +
        "); it would be dropped.";
    return false;
}

bool ValidateProxySettings(const ProxySettings & proxy, std::string & error)
{
    error.clear();
    if (!proxy.Active())
    {
        return true;
    }
    if (proxy.method == ProxyMethod::Local)
    {
        if (TrimAscii(proxy.command).empty())
        {
            error = "A local (Cmd) proxy needs a command: set "
                "ProxyTelnetCommand=COMMAND with %host and %port where the "
                "destination belongs.";
            return false;
        }
        return true;
    }
    if (TrimAscii(proxy.host).empty())
    {
        error = std::string("The ") + ProxyMethodName(proxy.method) +
            " proxy has no host: set ProxyHost=HOST.";
        return false;
    }
    if (proxy.port != 0 && (proxy.port < 1 || proxy.port > 65535))
    {
        error = "The proxy port must be between 1 and 65535.";
        return false;
    }
    if (proxy.method == ProxyMethod::Telnet && TrimAscii(proxy.command).empty())
    {
        error = "A telnet proxy needs its command: set "
            "ProxyTelnetCommand=COMMAND (upstream's default is "
            "'connect %host %port\\n').";
        return false;
    }
    return true;
}

bool ProxyUsableByCurl(const ProxySettings & proxy, std::string & error)
{
    error.clear();
    if (!proxy.Active())
    {
        return true;
    }
    if (proxy.method == ProxyMethod::Telnet || proxy.method == ProxyMethod::Local)
    {
        error = std::string("This protocol connects through curl, which cannot "
            "run a ") +
            (proxy.method == ProxyMethod::Telnet ? "telnet" : "local command") +
            " proxy; use SOCKS4, SOCKS5 or HTTP, or a protocol this build "
            "serves with its own transport.";
        return false;
    }
    return true;
}

namespace {

bool ParsePortNumber(const std::string & text, int & port)
{
    if (text.empty())
    {
        return false;
    }
    unsigned int value = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < '0' || c > '9')
        {
            return false;
        }
        const unsigned int digit = c - '0';
        if (value > (65535U - digit) / 10U)
        {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value == 0)
    {
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

int HexDigit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool PercentDecode(const std::string & input, std::string & output)
{
    output.clear();
    for (size_t i = 0; i < input.size(); i++)
    {
        if (input[i] != '%')
        {
            output += input[i];
            continue;
        }
        if (i + 2 >= input.size())
        {
            return false;
        }
        const int high = HexDigit(static_cast<unsigned char>(input[i + 1]));
        const int low = HexDigit(static_cast<unsigned char>(input[i + 2]));
        if (high < 0 || low < 0)
        {
            return false;
        }
        const char decoded = static_cast<char>((high << 4) | low);
        if (decoded == '\0')
        {
            return false;
        }
        output += decoded;
        i += 2;
    }
    return true;
}

bool HasWhitespaceOrControl(const std::string & text)
{
    for (size_t i = 0; i < text.size(); i++)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c <= 0x20 || c == 0x7f)
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool ParseSiteUrl(const std::string & url, SessionOptions & options, std::string & error)
{
    error.clear();
    SessionOptions parsed = options;
    parsed.hostName.clear();
    parsed.initialDirectory.clear();

    std::string rest = url;
    std::string scheme;
    std::string::size_type pos = rest.find("://");
    const bool hadScheme = (pos != std::string::npos);
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
        scheme[i] = static_cast<char>(tolower(static_cast<unsigned char>(scheme[i])));
    }
    if (scheme != "sftp" && scheme != "ssh" && scheme != "scp" &&
        scheme != "ftp" && scheme != "ftps" && scheme != "http" &&
        scheme != "https" && scheme != "s3" && scheme != "dav" &&
        scheme != "webdav")
    {
        error = "Unsupported protocol '" + scheme + "'.";
        return false;
    }
    if (url.empty())
    {
        error = "No host name in site URL.";
        return false;
    }

    parsed.protocol = scheme;
    int defaultPort = 22;
    if (scheme == "ftp")
    {
        defaultPort = 21;
    }
    else if (scheme == "ftps")
    {
        defaultPort = 990;
    }
    else if (scheme == "http" || scheme == "dav")
    {
        defaultPort = 80;
    }
    else if (scheme == "https" || scheme == "s3" || scheme == "webdav")
    {
        defaultPort = 443;
    }
    bool explicitPort = false;

    /* Query and fragment syntax is not meaningful for an SFTP site. */
    if (rest.find('?') != std::string::npos || rest.find('#') != std::string::npos)
    {
        error = "Site URLs must not contain a query or fragment; encode reserved characters with percent escapes.";
        return false;
    }

    /* Split off the path and decode its URI escapes. */
    std::string path;
    pos = rest.find('/');
    if (pos != std::string::npos)
    {
        const std::string encodedPath = rest.substr(pos);
        rest = rest.substr(0, pos);
        if (!PercentDecode(encodedPath, path))
        {
            error = "Invalid percent escape in site URL path.";
            return false;
        }
    }

    /* Split off credentials. A literal '@' in user information must be escaped. */
    std::string::size_type at = rest.find('@');
    if (at != std::string::npos)
    {
        if (rest.find('@', at + 1) != std::string::npos)
        {
            error = "Invalid site URL authority; escape '@' in credentials as '%40'.";
            return false;
        }
        const std::string auth = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const std::string::size_type colon = auth.find(':');
        std::string user;
        std::string password;
        const std::string encodedUser = auth.substr(0, colon);
        if (encodedUser.empty() || !PercentDecode(encodedUser, user) ||
            user.empty())
        {
            error = "Invalid user name in site URL.";
            return false;
        }
        parsed.userName = user;
        if (colon != std::string::npos)
        {
            if (!PercentDecode(auth.substr(colon + 1), password))
            {
                error = "Invalid percent escape in site URL credentials.";
                return false;
            }
            parsed.password = password;
        }
    }

    /* Host and port. IPv6 literals must be bracketed. */
    std::string encodedHost;
    if (!rest.empty() && rest[0] == '[')
    {
        const std::string::size_type close = rest.find(']');
        if (close == std::string::npos || close == 1)
        {
            error = "Malformed bracketed IPv6 address in site URL.";
            return false;
        }
        encodedHost = rest.substr(1, close - 1);
        const std::string tail = rest.substr(close + 1);
        if (!tail.empty())
        {
            if (tail[0] != ':' || !ParsePortNumber(tail.substr(1), parsed.portNumber))
            {
                error = "Invalid port or characters after bracketed host in site URL.";
                return false;
            }
            explicitPort = true;
        }
    }
    else
    {
        const std::string::size_type colon = rest.find(':');
        if (colon != std::string::npos)
        {
            if (rest.find(':', colon + 1) != std::string::npos)
            {
                error = "IPv6 addresses in site URLs must be enclosed in brackets.";
                return false;
            }
            encodedHost = rest.substr(0, colon);
            if (!ParsePortNumber(rest.substr(colon + 1), parsed.portNumber))
            {
                error = "Invalid port number in site URL.";
                return false;
            }
            explicitPort = true;
        }
        else
        {
            encodedHost = rest;
        }
    }

    if (!explicitPort)
    {
        if (hadScheme && (scheme == "ftp" || scheme == "ftps" || scheme == "http" ||
                          scheme == "https" || scheme == "dav" || scheme == "webdav"))
        {
            parsed.portNumber = defaultPort;
        }
        else if (options.portNumber > 0)
        {
            parsed.portNumber = options.portNumber;
        }
        else
        {
            parsed.portNumber = defaultPort;
        }
    }

    if (!PercentDecode(encodedHost, parsed.hostName))
    {
        error = "Invalid percent escape in site URL host name.";
        return false;
    }
    if (parsed.hostName.empty())
    {
        error = "No host name in site URL.";
        return false;
    }
    if (HasWhitespaceOrControl(parsed.hostName))
    {
        error = "Invalid whitespace or control character in site URL host name.";
        return false;
    }
    if (parsed.portNumber <= 0 || parsed.portNumber > 65535)
    {
        error = "Invalid port number in site URL.";
        return false;
    }

    parsed.initialDirectory = path;
    options = parsed;
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
    mReceivedEnd(0),
    mOutputSink(NULL),
    mOutputSinkCtx(NULL)
{
}

void Session::SetOutputSink(OutputSinkFn sink, void * ctx)
{
    mOutputSink = sink;
    mOutputSinkCtx = ctx;
}

void Session::DeliverChannelData(const void * data, size_t len)
{
    if (mOutputSink != NULL)
    {
        /* An exec session prints its command output instead of feeding
         * the SFTP protocol layer. */
        mOutputSink(mOutputSinkCtx, static_cast<const char *>(data), len);
        return;
    }
    PushReceivedData(data, len);
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
    /*
     * A proxy is checked before anything is connected: a configuration the
     * transport cannot honour would otherwise turn into a direct connection
     * the session never asked for.
     */
    if (!ValidateProxySettings(options.proxy, error))
    {
        return false;
    }
    if (!UseNativeSftp(options.protocol))
    {
        /* A curl-backed session has no connection of its own; it must
         * still become the current session, because ProtocolClient
         * reads its options for every operation. */
        if (options.hostName.empty())
        {
            error = "No host name was specified.";
            return false;
        }
        if (!ProxyUsableByCurl(options.proxy, error))
        {
            return false;
        }
        /*
         * Nothing here proves the host is there, so ask it once, now: an
         * 'open' of a dead port or a misspelled host has to fail here, as a
         * real connection does, and not turn into a puzzle for the first
         * command. Only an endpoint that cannot be reached counts against
         * us; a directory the server refuses to list is the command's
         * business.
         */
        if (!ProtocolClient::CheckEndpoint(options, options.initialDirectory, error))
        {
            return false;
        }
        Close();
        mOptions = options;
        mUserName = options.userName;
        mPasswordUsed = false;
        mAlive = true;
        mConnectionError.clear();
        mOpen = true;
        SetCurrentSession(this);
        Console::Instance().LogEvent("Connected via " + mOptions.protocol +
            " to " + mOptions.hostName);
        return true;
    }
    return Connect(options, "sftp", true, true, error);
}

bool Session::Connect(const SessionOptions & options, const char * program,
    bool subsystem, bool takeCurrent, std::string & error)
{
    if (options.hostName.empty())
    {
        error = "No host name was specified.";
        return false;
    }
    if (options.portNumber <= 0 || options.portNumber > 65535)
    {
        error = "The port number must be between 1 and 65535.";
        return false;
    }
    if (options.timeoutSecs <= 0 || options.timeoutSecs > INT_MAX / 1000)
    {
        error = "The connection timeout must be a positive number of seconds.";
        return false;
    }
    if (!ValidateProxySettings(options.proxy, error))
    {
        return false;
    }

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
        std::string keyError;
        const std::string keyFile = PreparePrivateKeyFile(
            ExpandTilde(mOptions.privateKeyFile), keyError);
        if (keyFile.empty())
        {
            error = keyError;
            Close();
            return false;
        }
        Filename * fileName = filename_from_str(keyFile.c_str());
        conf_set_filename(conf, CONF_keyfile, fileName);
        filename_free(fileName);
    }
    /* Run the SFTP subsystem, like psftp does, or the given command
     * through an 'exec' request. */
    conf_set_bool(conf, CONF_ssh_subsys, subsystem);
    conf_set_str(conf, CONF_remote_cmd, program);
    conf_set_bool(conf, CONF_ssh_simple, true);
    conf_set_bool(conf, CONF_nopty, true);
    conf_set_int(conf, CONF_connect_timeout, mOptions.timeoutSecs * 1000);
    conf_set_bool(conf, CONF_ssh_show_banner, true);
    conf_set_int(conf, CONF_proxy_log_to_term, FORCE_OFF);

    /*
     * The proxy of the session, under the same names upstream's session
     * settings use. PuTTY's core does the negotiation (SOCKS 4 and 5, HTTP
     * CONNECT, a telnet command or a local command), so this is only a
     * translation of the settings into the core's configuration.
     */
    if (mOptions.proxy.Active())
    {
        const ProxySettings & proxy = mOptions.proxy;
        int type = PROXY_NONE;
        switch (proxy.method)
        {
        case ProxyMethod::Socks4: type = PROXY_SOCKS4; break;
        case ProxyMethod::Socks5: type = PROXY_SOCKS5; break;
        case ProxyMethod::Http:   type = PROXY_HTTP;   break;
        case ProxyMethod::Telnet: type = PROXY_TELNET; break;
        case ProxyMethod::Local:  type = PROXY_CMD;    break;
        default:                  type = PROXY_NONE;   break;
        }
        conf_set_int(conf, CONF_proxy_type, type);
        conf_set_str(conf, CONF_proxy_host, proxy.host.c_str());
        conf_set_int(conf, CONF_proxy_port, proxy.EffectivePort());
        conf_set_str(conf, CONF_proxy_username, proxy.userName.c_str());
        conf_set_str(conf, CONF_proxy_password, proxy.password.c_str());
        conf_set_str(conf, CONF_proxy_telnet_command, proxy.command.c_str());
        conf_set_str(conf, CONF_proxy_exclude_list, proxy.excludeList.c_str());
        conf_set_int(conf, CONF_proxy_dns,
            proxy.dns == 0 ? FORCE_OFF : proxy.dns == 2 ? FORCE_ON : AUTO);
        conf_set_bool(conf, CONF_even_proxy_localhost, proxy.evenLocalhost);

        Console::Instance().LogEvent(std::string("Connecting through the ") +
            ProxyMethodName(proxy.method) + " proxy " +
            (proxy.method == ProxyMethod::Local ?
                proxy.command : proxy.host + ":" + std::to_string(proxy.EffectivePort())));
    }

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

    if (takeCurrent)
    {
        SetCurrentSession(this);
    }

    Console::Instance().LogEvent(subsystem ?
        "Waiting for the SSH channel to open" :
        "Waiting for the command to start");
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
    /* A failed transfer can leave request IDs queued in the SFTP core. The
     * connection is being discarded, so release that state before another
     * session can start reusing it. */
    lscp_sftp_cleanup();
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
    /* A key file imported from the OpenSSH format is only needed while
     * the session is open. */
    DiscardImportedPrivateKeyFile();
    mReceived.clear();
    mReceivedStart = 0;
    mReceivedEnd = 0;
    mOpen = false;
    mAlive = false;
}

namespace {

void ExecOutputSink(void * ctx, const char * data, size_t len)
{
    (void)ctx;
    Console::Instance().Write(std::string(data, len));
}

} // namespace

bool Session::ExecRemote(const SessionOptions & options,
    const std::string & command, std::string & error)
{
    if (!UseNativeSftp(options.protocol))
    {
        error = "Remote command execution needs an SSH (sftp://) session.";
        return false;
    }
    /* A one-shot session of its own: it never becomes the session the
     * SFTP transport glue addresses, so the open session of the script
     * stays connected and buffered while the command runs. */
    Session exec;
    exec.SetOutputSink(ExecOutputSink, NULL);
    if (!exec.Connect(options, command.c_str(), false, false, error))
    {
        return false;
    }
    while (exec.IsAlive())
    {
        if (!exec.Pump())
        {
            break;
        }
    }
    exec.Close();
    if (!exec.ConnectionError().empty())
    {
        error = exec.ConnectionError();
        return false;
    }
    return true;
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
