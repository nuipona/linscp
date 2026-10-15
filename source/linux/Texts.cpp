#include "Texts.h"

#include "Platform.h"

#include <cstdio>

#if !defined(_WIN32)
/* from source/putty/utils/version.c (puttyexp.h) */
extern "C" const char * get_putty_version(void);
#endif

namespace linuxscp {
namespace Texts {

const char * const ProductName = "LinuxSCP";
const char * const ProductVersion = "1.0";

namespace {

std::string Join(const std::string & left, const std::string & right)
{
    return left + right;
}

} // namespace

std::string VersionLine()
{
    std::string result = std::string(ProductName) + " " + ProductVersion;
#if defined(_WIN32)
    result += " (Windows, curl transfer backend)";
#else
    result += " (PuTTY core ";
    result += get_putty_version();
    result += ")";
#endif
    return result;
}

std::string Usage()
{
    return
"LinuxSCP " + std::string(ProductVersion) + " - secure file transfer client\n"
"\n"
"  linuxscp [options] [sftp://[user[:password]@]host[:port]/[path]]\n"
"\n"
"Options:\n"
"  -script=FILE      execute commands from FILE ('-' reads stdin)\n"
"  -command=CMD      execute commands (separate several with ';')\n"
"  -hostkey=KEY      pin the host key (fingerprint or '*' for this connection)\n"
"  -password=PWD     password to use (prefer the LINUXSCP_PASSWORD variable)\n"
"  -privatekey=FILE  private key file for public key authentication\n"
"  -username=NAME    user name to authenticate with\n"
"  -port=NUMBER      port to connect to (default 22)\n"
"  -timeout=SECS     connection timeout in seconds (default 15)\n"
"  -log=FILE         log the session to FILE\n"
"  -xmllog=FILE      write a structured (XML) action log of every command,\n"
"                    message and transfer to FILE (passwords masked)\n"
"  -batch            never ask, abort instead\n"
"  -parameter VAL..  script parameters, for %1% .. %9%, %TIMESTAMP% and\n"
"                    %ENVIRONMENT% expansion in the commands\n"
"  -gui              launch the graphical Commander desktop interface\n"
"  -server           command server of the graphical client: one command\n"
"                    per line on stdin, 'LSCP-RESULT <code>' after each\n"
"  -verbose          print SSH event log messages\n"
"  -version          print the version and exit\n"
"  -help             print this help and exit\n"
"\n"
"Commands:\n"
"  open SITE         connect to a site (sftp://user@host:port/path)\n"
"  close [INDEX]     disconnect the open session, or session INDEX\n"
"  cd DIR            change the remote directory\n"
"  pwd               print the remote directory\n"
"  ls [-al1] [DIR]   list remote files\n"
"  lcd DIR           change the local directory\n"
"  lpwd              print the local directory\n"
"  lls [DIR[/MASK]]  list local files with date, time and size\n"
"  get [opts] REMOTE [LOCAL]  download file(s) or directories\n"
"  put [opts] LOCAL [REMOTE]  upload file(s) or directories\n"
"                    both: -r -delete -neweronly -resume -append -preservetime\n"
"                    -speed=KB -transfer=binary|ascii|automatic -filemask=MASK\n"
"                    -resumesupport=on|off|KB -rawtransfersettings[=N]\n"
"                    -permissions[=OCTAL] -nopermissions\n"
"  synchronize local|remote|both [LOCAL [REMOTE]] [switches]\n"
"                    synchronize directories: -delete -preview\n"
"                    -nonrecursive -filemask=MASK\n"
"                    -criteria=time|size|checksum|either|both|none\n"
"  keepuptodate [LOCAL [REMOTE]] [switches]\n"
"                    reflect local changes on the remote side; Linux and\n"
"                    Windows watch locally, others poll -interval=SECS\n"
"                    (-count=N bounds the passes; Ctrl-C stops watching)\n"
"  edit FILE         edit a remote file in an external editor\n"
"                    (-force, -keep, -editor=COMMAND)\n"
"  session [INDEX]   list the open sessions, or switch to INDEX\n"
"  sessions [NAME]   list stored sites; NAME opens one, and\n"
"                    (also: sessions export FILE, sessions import FILE)\n"
"  savesession NAME  store the current connection as a named session\n"
"                    (-preset=NAME stores its transfer preset)\n"
"  rmsession NAME    forget a stored session\n"
"  presets           list the transfer presets; savepreset NAME K=V...\n"
"                    stores one, rmspreset NAME removes one\n"
"  rm FILE           remove remote file(s)\n"
"  rmdir DIR         remove a remote directory\n"
"  mkdir DIR         create a remote directory\n"
"  mv FROM TO        move/rename a remote file\n"
"  chmod MODE FILE [FILE...] [-recursive]\n"
"                    change remote permissions; MODE is octal (644) or\n"
"                    symbolic (u+x, a=r,go-w)\n"
"  copyid [KEYFILE]  add the public key of KEYFILE, or of the session key,\n"
"                    to the server's ~/.ssh/authorized_keys\n"
"  stat FILE         print the listing line of a remote path\n"
"  attr FILE         print remote file attributes, key/value\n"
"  terminal          open an interactive shell on the server\n"
"  echo [MESSAGE]    display text message\n"
"  option confirm|batch|resume|editor|editmaxsize|transfer|preset VALUE\n"
"                    configure session options (preset NAME makes a\n"
"                    transfer preset the default, 'option preset off' clears)\n"
"  binary / ascii    select the transfer mode of the following commands\n"
"  help              list the commands (alias: man)\n"
"  exit              end the session\n";
}

std::string CannotOpenSession(const std::string & host, const std::string & reason)
{
    return "Cannot open session to " + host + ": " + reason;
}

std::string ConnectionClosed()
{
    return "Connection has been unexpectedly closed.";
}

std::string HostKeyUnknown(const std::string & host, int port,
    const std::string & keyType, const std::string & fingerprint)
{
    char portText[16];
    snprintf(portText, sizeof(portText), "%d", port);
    return "The host key of " + host + " (port " + portText + ") is not cached.\n" +
        "  " + keyType + " key fingerprint: " + fingerprint + "\n" +
        "If you expected a different fingerprint, do not continue.";
}

std::string HostKeyMismatch(const std::string & host, int port,
    const std::string & keyType, const std::string & fingerprint)
{
    char portText[16];
    snprintf(portText, sizeof(portText), "%d", port);
    return "The host key of " + host + " (port " + portText + ") does not match the cached key.\n" +
        "  " + keyType + " key fingerprint: " + fingerprint + "\n" +
        "The connection may be intercepted; refusing to continue.";
}

std::string HostKeyUnexpected(const std::string & host, int port,
    const std::string & keyType, const std::string & fingerprint,
    const std::string & expected)
{
    char portText[16];
    snprintf(portText, sizeof(portText), "%d", port);
    return "The host key of " + host + " (port " + portText + ") is not the expected one.\n" +
        "  " + keyType + " key fingerprint: " + fingerprint + "\n" +
        "  expected fingerprint:           " + expected + "\n" +
        "The connection may be intercepted; refusing to continue.";
}

std::string HostKeyCached(const std::string & host, int port)
{
    char portText[16];
    snprintf(portText, sizeof(portText), "%d", port);
    return Join("Host key of " + host + " (port " + portText + ") verified from cache.", "");
}

std::string PasswordPrompt(const std::string & userName, const std::string & host)
{
    std::string result = userName;
    result += "@" + host + "'s password: ";
    return result;
}

std::string NotConnected()
{
    return "Not connected. Use 'open' to connect to a site first.";
}

std::string ReadingDirectory(const std::string & path)
{
    return "Reading directory " + path + "...";
}

std::string DirectoryRead(int count)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d entries", count);
    return std::string(buffer);
}

std::string CannotReadDirectory(const std::string & path, const std::string & reason)
{
    return "Cannot read directory '" + path + "'. " + reason;
}

std::string RetrievingFile(const std::string & remote, const std::string & local)
{
    return "Retrieving " + remote + " to " + local;
}

std::string UploadingFile(const std::string & local, const std::string & remote)
{
    return "Uploading " + local + " to " + remote;
}

std::string FileTransferred(const std::string & name, uint64_t size)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), " [%llu bytes]",
        static_cast<unsigned long long>(size));
    return Join(name, buffer);
}

std::string TransferFailed(const std::string & name, const std::string & reason)
{
    return "Transfer of '" + name + "' failed. " + reason;
}

std::string RemoteError(const std::string & operation, const std::string & path,
    const std::string & reason)
{
    return "Cannot " + operation + " '" + path + "'. " + reason;
}

std::string LocalError(const std::string & operation, const std::string & path,
    const std::string & reason)
{
    return "Local " + operation + " of '" + path + "' failed. " + reason;
}

std::string UnknownCommand(const std::string & command)
{
    return "Unknown command '" + command + "'. Type 'help' for a list of commands.";
}

std::string WrongArguments(const std::string & command, const std::string & hint)
{
    return "Wrong arguments of '" + command + "' command. " + hint;
}

} // namespace Texts
} // namespace linuxscp
