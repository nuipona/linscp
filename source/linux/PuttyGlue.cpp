/*
 * PuttyGlue: the platform layer the shared PuTTY core expects from its
 * front end, for the POSIX build.
 *
 * Three groups of functions live here:
 *
 *  1. The MPEXT callback-set seam. The fork's core was built for
 *     Windows, where every SSH session owns a `struct callback_set'
 *     reached through get_callback_set()/get_seat_callback_set()
 *     (MPEXT means "one callback queue per session"). The console
 *     client has a single session, so all of these return one
 *     statically allocated set; the vendored POSIX platform layer
 *     reaches it through linuxscp_posix_callback_set().
 *
 *  2. Persistent storage (storage.h): saved sessions, the SSH host key
 *     cache and the random seed file, as plain files below the
 *     configuration directory.
 *
 *  3. The SFTP transport (sftp_senddata/sftp_recvdata/sftp_sendbuffer),
 *     which simply forwards to the session that is currently open.
 */

#include "Console.h"
#include "Session.h"
#include "Texts.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "PuttyCompat.h"

/*
 * storage.h declares these as opaque; here is what they are on POSIX.
 * They must be defined at global scope so that they complete the C typedefs
 * rather than shadowing them.
 */
struct settings_r
{
    std::vector<std::string> keys;
    std::vector<std::string> values;
};

struct settings_w
{
    std::string path;
    std::string contents;
};

struct host_ca_enum
{
    size_t index;
};

namespace {

/* ----------------------------------------------------------------------
 * Configuration directory
 */

std::string StorageRoot()
{
    const char * overrideDir = getenv("LINUXSCP_CONFIG_DIR");
    if (overrideDir != NULL && overrideDir[0] != '\0')
    {
        return std::string(overrideDir);
    }
    const char * xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] != '\0')
    {
        return std::string(xdg) + "/LinuxSCP";
    }
    const char * home = getenv("HOME");
    if (home != NULL && home[0] != '\0')
    {
        return std::string(home) + "/.config/LinuxSCP";
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "/tmp/LinuxSCP-%u",
        static_cast<unsigned int>(getuid()));
    return std::string(buffer);
}

bool MakeDirectoryTree(const std::string & path)
{
    std::string built;
    for (size_t i = 0; i < path.size(); i++)
    {
        built += path[i];
        if (path[i] == '/' && i > 0)
        {
            mkdir(built.c_str(), 0700);
        }
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
    {
        return false;
    }
    return true;
}

std::string StoragePath(const std::string & name)
{
    return StorageRoot() + "/" + name;
}

std::string HostKeyPath()
{
    return StoragePath("sshhostkeys");
}

std::string RandomSeedPath()
{
    return StoragePath("randomseed");
}

/* Session names are used as file names, so '/' cannot appear in them. */
std::string EscapeSessionName(const char * name)
{
    std::string result;
    for (const char * p = name; *p != '\0'; p++)
    {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')
        {
            result += *p;
        }
        else
        {
            char buffer[8];
            snprintf(buffer, sizeof(buffer), "%%%02X",
                static_cast<unsigned int>(static_cast<unsigned char>(*p)));
            result += buffer;
        }
    }
    return result;
}

bool ReadFile(const std::string & path, std::vector<char> & contents)
{
    contents.clear();
    FILE * file = fopen(path.c_str(), "rb");
    if (file == NULL)
    {
        return false;
    }
    char buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        contents.insert(contents.end(), buffer, buffer + read);
    }
    fclose(file);
    return true;
}

bool ReadLines(const std::string & path, std::vector<std::string> & lines)
{
    std::vector<char> contents;
    if (!ReadFile(path, contents))
    {
        return false;
    }
    std::string line;
    for (size_t i = 0; i < contents.size(); i++)
    {
        if (contents[i] == '\n')
        {
            lines.push_back(line);
            line.clear();
        }
        else if (contents[i] != '\r')
        {
            line += contents[i];
        }
    }
    if (!line.empty())
    {
        lines.push_back(line);
    }
    return true;
}

bool WriteFile(const std::string & path, const std::string & contents)
{
    MakeDirectoryTree(StorageRoot());
    FILE * file = fopen(path.c_str(), "wb");
    if (file == NULL)
    {
        return false;
    }
    if (!contents.empty())
    {
        if (fwrite(contents.data(), 1, contents.size(), file) != contents.size())
        {
            fclose(file);
            return false;
        }
    }
    fclose(file);
    chmod(path.c_str(), 0600);
    return true;
}

/* ----------------------------------------------------------------------
 * Saved sessions
 */

std::string SessionPath(const char * name)
{
    return StoragePath("sessions/") + EscapeSessionName(name);
}

/* ----------------------------------------------------------------------
 * UTF-8 conversions for BinarySink_put_mb_to_wc / put_wc_to_mb
 *
 * The Windows implementation understands the whole list of Windows code
 * pages; the POSIX front end works in UTF-8, which is both CP_UTF8 and
 * (on this platform) the default code page.
 */

bool IsUtf8CodePage(int codepage)
{
    return codepage == CP_UTF8 || codepage == DEFAULT_CODEPAGE || codepage == -1;
}

void PutUtf8(BinarySink * bs, unsigned long value)
{
    char buffer[8];
    size_t len = 0;
    if (value < 0x80)
    {
        buffer[len++] = static_cast<char>(value);
    }
    else if (value < 0x800)
    {
        buffer[len++] = static_cast<char>(0xC0 | (value >> 6));
        buffer[len++] = static_cast<char>(0x80 | (value & 0x3F));
    }
    else if (value < 0x10000)
    {
        buffer[len++] = static_cast<char>(0xE0 | (value >> 12));
        buffer[len++] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        buffer[len++] = static_cast<char>(0x80 | (value & 0x3F));
    }
    else
    {
        buffer[len++] = static_cast<char>(0xF0 | (value >> 18));
        buffer[len++] = static_cast<char>(0x80 | ((value >> 12) & 0x3F));
        buffer[len++] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        buffer[len++] = static_cast<char>(0x80 | (value & 0x3F));
    }
    put_data(bs, buffer, len);
}

} // namespace

/* ======================================================================
 * C entry points expected by the core
 * ==================================================================== */

extern "C" {

/* --- 1. The MPEXT callback-set seam ---------------------------------- */

struct callback_set * linuxscp_posix_callback_set(void)
{
    static struct callback_set callbackSet;
    static bool initialised = false;
    if (!initialised)
    {
        memset(&callbackSet, 0, sizeof(callbackSet));
        linuxscp_critical_section_init(&callbackSet.ready_critsec[0]);
        initialised = true;
    }
    return &callbackSet;
}

struct callback_set * get_callback_set(Plug * plug)
{
    return linuxscp_posix_callback_set();
}

struct callback_set * get_seat_callback_set(Seat * seat)
{
    return linuxscp_posix_callback_set();
}

struct callback_set * get_log_callback_set(LogContext * ctx)
{
    return linuxscp_posix_callback_set();
}

Seat * get_log_seat(LogContext * ctx)
{
    linuxscp::Session * session = linuxscp::CurrentSession();
    return session != NULL ? static_cast<Seat *>(session->SeatHandle()) : NULL;
}

/* Protects the random pool and the bignum code; see platform.h. The
 * critical sections must be recursive (sshrand.c re-enters the section
 * from the noise callback), so they are set up here rather than with a
 * static initialiser. */
CRITICAL_SECTION putty_section;

/* One-time setup of the recursive mutex type, before main() does anything
 * that could take a section. */
void linuxscp_platform_init(void)
{
    linuxscp_critical_section_init(&putty_section);
}

} /* extern "C" */

namespace {

pthread_mutexattr_t g_criticalSectionAttr;
bool g_criticalSectionAttrReady = false;

const pthread_mutexattr_t & CriticalSectionAttributes()
{
    if (!g_criticalSectionAttrReady)
    {
        pthread_mutexattr_init(&g_criticalSectionAttr);
        pthread_mutexattr_settype(&g_criticalSectionAttr, PTHREAD_MUTEX_RECURSIVE);
        g_criticalSectionAttrReady = true;
    }
    return g_criticalSectionAttr;
}

} // namespace

extern "C" {

void linuxscp_critical_section_init(CRITICAL_SECTION * cs)
{
    pthread_mutex_init(cs, &CriticalSectionAttributes());
}

void linuxscp_critical_section_enter(CRITICAL_SECTION * cs)
{
    pthread_mutex_lock(cs);
}

void linuxscp_critical_section_leave(CRITICAL_SECTION * cs)
{
    pthread_mutex_unlock(cs);
}

/* The SSH local version string, sent in the version exchange. In the
 * Windows build this comes from utils/version.c; there it is left out of
 * FORK_BUILD, so the front end provides it. ssh.h declares it without
 * 'const' when MPEXT is set. */
char sshver[] = "LinuxSCP-1.0";

/* Connection sharing is supported by the vendored unix/sharing.c. */
const bool share_can_be_upstream = true;
const bool share_can_be_downstream = true;

NORETURN void modalfatalbox(const char * format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    linuxscp::Console::Instance().Fatal(buffer);
    exit(1);
}

/* timing.c announces the next timer through this; cliloop.c recomputes the
 * timers on every iteration of the main loop, so there is nothing to do. */
void timer_change_notify(unsigned long next)
{
}

void ldisc_echoedit_update(Ldisc * ldisc)
{
    /* No terminal discipline is used: there is no interactive shell. */
}

const char * in_memory_key_data(const Filename * fn)
{
    /* Only the Windows build can embed a private key in the binary. */
    return NULL;
}

void old_keyfile_warning(void)
{
    linuxscp::Console::Instance().Error(
        "Warning: this private key is in an old format; consider converting it.");
}

/* --- 2. Persistent storage ------------------------------------------- */

settings_r * open_settings_r(const char * name)
{
    if (name == NULL)
    {
        return NULL;
    }
    std::vector<std::string> lines;
    if (!ReadLines(SessionPath(name), lines))
    {
        return NULL;
    }
    settings_r * handle = new settings_r();
    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string::size_type pos = lines[i].find('=');
        if (pos == std::string::npos)
        {
            continue;
        }
        handle->keys.push_back(lines[i].substr(0, pos));
        handle->values.push_back(lines[i].substr(pos + 1));
    }
    return handle;
}

void close_settings_r(settings_r * handle)
{
    delete handle;
}

char * read_setting_s(settings_r * handle, const char * key)
{
    if (handle == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < handle->keys.size(); i++)
    {
        if (handle->keys[i] == key)
        {
            return dupstr(handle->values[i].c_str());
        }
    }
    return NULL;
}

int read_setting_i(settings_r * handle, const char * key, int defvalue)
{
    char * value = read_setting_s(handle, key);
    if (value == NULL)
    {
        return defvalue;
    }
    int result = atoi(value);
    sfree(value);
    return result;
}

Filename * read_setting_filename(settings_r * handle, const char * key)
{
    char * value = read_setting_s(handle, key);
    if (value == NULL)
    {
        return NULL;
    }
    Filename * result = filename_from_str(value);
    sfree(value);
    return result;
}

FontSpec * read_setting_fontspec(settings_r * handle, const char * key)
{
    /* Font settings are meaningless on this platform. */
    return NULL;
}

settings_w * open_settings_w(const char * name, char ** errmsg)
{
    if (errmsg != NULL)
    {
        *errmsg = NULL;
    }
    if (name == NULL)
    {
        return NULL;
    }
    MakeDirectoryTree(StoragePath("sessions"));
    settings_w * handle = new settings_w();
    handle->path = SessionPath(name);
    return handle;
}

void write_setting_s(settings_w * handle, const char * key, const char * value)
{
    if (handle == NULL)
    {
        return;
    }
    handle->contents += key;
    handle->contents += "=";
    handle->contents += (value != NULL ? value : "");
    handle->contents += "\n";
}

void write_setting_i(settings_w * handle, const char * key, int value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    write_setting_s(handle, key, buffer);
}

void write_setting_filename(settings_w * handle, const char * key, Filename * value)
{
    write_setting_s(handle, key, value != NULL ? filename_to_str(value) : NULL);
}

void write_setting_fontspec(settings_w * handle, const char * key, FontSpec * font)
{
    /* Nothing to store: see read_setting_fontspec. */
}

void close_settings_w(settings_w * handle)
{
    if (handle == NULL)
    {
        return;
    }
    WriteFile(handle->path, handle->contents);
    delete handle;
}

/* --- SSH host key cache ---------------------------------------------- */

static std::string HostKeyPrefix(const char * hostname, int port)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", port);
    return std::string(hostname) + "@" + buffer;
}

int retrieve_host_key(const char * hostname, int port, const char * keytype,
    char * key, int maxlen)
{
    std::vector<std::string> lines;
    ReadLines(HostKeyPath(), lines);
    std::string prefix = HostKeyPrefix(hostname, port);
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (lines[i].compare(0, prefix.size(), prefix) == 0)
        {
            std::string rest = lines[i].substr(prefix.size());
            std::string::size_type pos = rest.find(' ');
            if (pos == std::string::npos)
            {
                continue;
            }
            std::string type = rest.substr(0, pos);
            std::string value = rest.substr(pos + 1);
            if (keytype != NULL && type != keytype)
            {
                continue;
            }
            if (key != NULL && maxlen > 0)
            {
                strncpy(key, value.c_str(), maxlen - 1);
                key[maxlen - 1] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

bool have_ssh_host_key(Seat * seat, const char * host, int port, const char * keytype)
{
    return retrieve_host_key(host, port, keytype, NULL, 0) != 0;
}

void store_host_key(Seat * seat, const char * hostname, int port,
    const char * keytype, const char * key)
{
    std::vector<std::string> lines;
    ReadLines(HostKeyPath(), lines);
    std::string prefix = HostKeyPrefix(hostname, port);
    std::string entry = prefix + " " + keytype + " " + key;
    bool replaced = false;
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (lines[i].compare(0, prefix.size(), prefix) == 0)
        {
            std::string rest = lines[i].substr(prefix.size());
            std::string::size_type pos = rest.find(' ');
            if (pos != std::string::npos && rest.substr(0, pos) == keytype)
            {
                lines[i] = entry;
                replaced = true;
            }
        }
    }
    if (!replaced)
    {
        lines.push_back(entry);
    }
    std::string contents;
    for (size_t i = 0; i < lines.size(); i++)
    {
        contents += lines[i];
        contents += "\n";
    }
    WriteFile(HostKeyPath(), contents);
}

/* --- Trusted certification authorities ------------------------------- */

host_ca_enum * enum_host_ca_start(void)
{
    return new host_ca_enum();
}

bool enum_host_ca_next(host_ca_enum * handle, strbuf * out)
{
    /* No CA store on this platform yet. */
    return false;
}

void enum_host_ca_finish(host_ca_enum * handle)
{
    delete handle;
}

host_ca * host_ca_load(const char * name)
{
    return NULL;
}

/* --- Random seed ----------------------------------------------------- */

void read_random_seed(noise_consumer_t consumer)
{
    std::vector<char> contents;
    if (!ReadFile(RandomSeedPath(), contents))
    {
        return;
    }
    if (!contents.empty())
    {
        consumer(contents.data(), static_cast<int>(contents.size()));
    }
}

void write_random_seed(void * data, int len)
{
    if (len <= 0)
    {
        return;
    }
    WriteFile(RandomSeedPath(),
        std::string(static_cast<char *>(data), static_cast<size_t>(len)));
}

/* --- Platform defaults ----------------------------------------------- */

char * platform_default_s(const char * name)
{
    /* No platform overrides: the compiled-in defaults from conf.h win. */
    return NULL;
}

bool platform_default_b(const char * name, bool def)
{
    return def;
}

int platform_default_i(const char * name, int def)
{
    return def;
}

/* These must never return NULL: settings.c passes the result straight to
 * conf_set_filename()/conf_set_fontspec(), which copy it. An empty
 * Filename is the "no file" value (see filename_is_null). */
Filename * platform_default_filename(const char * name)
{
    if (strcmp(name, "LogFileName") == 0)
    {
        return filename_from_str("linuxscp.log");
    }
    return filename_from_str("");
}

FontSpec * platform_default_fontspec(const char * name)
{
    return fontspec_new_default();
}

/* --- Code page conversion -------------------------------------------- */

bool BinarySink_put_mb_to_wc(BinarySink * bs, int codepage, const char * mbstr, int mblen)
{
    if (mblen <= 0)
    {
        return true;
    }
    if (!IsUtf8CodePage(codepage))
    {
        return false;
    }
    int i = 0;
    while (i < mblen)
    {
        unsigned char first = static_cast<unsigned char>(mbstr[i]);
        int extra;
        unsigned long value;
        if (first < 0x80)
        {
            value = first;
            extra = 0;
        }
        else if ((first & 0xE0) == 0xC0)
        {
            value = first & 0x1F;
            extra = 1;
        }
        else if ((first & 0xF0) == 0xE0)
        {
            value = first & 0x0F;
            extra = 2;
        }
        else if ((first & 0xF8) == 0xF0)
        {
            value = first & 0x07;
            extra = 3;
        }
        else
        {
            value = 0xFFFD;
            extra = 0;
        }
        i++;
        for (int n = 0; n < extra && i < mblen; n++, i++)
        {
            unsigned char cont = static_cast<unsigned char>(mbstr[i]);
            if ((cont & 0xC0) != 0x80)
            {
                i--;
                break;
            }
            value = (value << 6) | (cont & 0x3F);
        }
        wchar_t wide = static_cast<wchar_t>(value);
        put_data(bs, &wide, sizeof(wide));
    }
    return true;
}

bool BinarySink_put_wc_to_mb(BinarySink * bs, int codepage, const wchar_t * wcstr,
    int wclen, const char * defchr)
{
    if (wclen <= 0)
    {
        return true;
    }
    if (!IsUtf8CodePage(codepage))
    {
        return false;
    }
    for (int i = 0; i < wclen; i++)
    {
        unsigned long value = static_cast<unsigned long>(wcstr[i]);
        if (value > 0x10FFFF)
        {
            if (defchr != NULL)
            {
                put_data(bs, defchr, strlen(defchr));
            }
            continue;
        }
        PutUtf8(bs, value);
    }
    return true;
}

/* --- 3. The SFTP transport ------------------------------------------- */

bool sftp_senddata(const char * data, size_t len)
{
    linuxscp::Session * session = linuxscp::CurrentSession();
    if (session == NULL)
    {
        return false;
    }
    return session->SendData(data, len);
}

size_t sftp_sendbuffer(void)
{
    linuxscp::Session * session = linuxscp::CurrentSession();
    if (session == NULL)
    {
        return 0;
    }
    return session->SendBufferSize();
}

bool sftp_recvdata(char * data, size_t len)
{
    linuxscp::Session * session = linuxscp::CurrentSession();
    if (session == NULL)
    {
        return false;
    }
    return session->ReceiveData(data, len);
}

} /* extern "C" */
