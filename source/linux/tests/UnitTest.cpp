/*
 * Unit tests for the platform-neutral parts of the LinuxSCP front end:
 * command line splitting, path handling, file masks, the output
 * formatting helpers, site URL parsing and local file system access.
 *
 * Run them with 'make -C source/linux check'.
 *
 * The connection code is not covered here; it needs a server, and is
 * exercised end to end instead (see docs/debian.md).
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <filesystem>

#include <map>

#include "PathUtil.h"
#include "LocalFs.h"
#include "Platform.h"
#include "Session.h"
#include "Script.h"
#include "Console.h"
#include "Editor.h"
#include "SessionStore.h"
#include "SyncPlanner.h"

namespace {

int gFailures = 0;
int gChecks = 0;

void ReportFailure(const char * file, int line, const std::string & what)
{
    gFailures++;
    fprintf(stderr, "%s:%d: %s\n", file, line, what.c_str());
}

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        gChecks++;                                                            \
        const std::string got = (actual);                                     \
        const std::string want = (expected);                                  \
        if (got != want)                                                      \
        {                                                                     \
            ReportFailure(__FILE__, __LINE__, std::string("expected '") +     \
                want + "', got '" + got + "'");                               \
        }                                                                     \
    } while (false)

#define CHECK_TRUE(expr)                                                      \
    do {                                                                      \
        gChecks++;                                                            \
        if (!(expr))                                                          \
        {                                                                     \
            ReportFailure(__FILE__, __LINE__, "expected true: " #expr);       \
        }                                                                     \
    } while (false)

#define CHECK_FALSE(expr)                                                     \
    do {                                                                      \
        gChecks++;                                                            \
        if (expr)                                                             \
        {                                                                     \
            ReportFailure(__FILE__, __LINE__, "expected false: " #expr);      \
        }                                                                     \
    } while (false)

using namespace linuxscp;

void SetEnvVar(const char * name, const char * value)
{
#if defined(_WIN32)
    _putenv_s(name, value != NULL ? value : "");
#else
    if (value != NULL)
    {
        setenv(name, value, 1);
    }
    else
    {
        unsetenv(name);
    }
#endif
}

std::string MakeTempDirectory()
{
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        return std::string();
    }
    for (int attempt = 0; attempt < 100; attempt++)
    {
        std::filesystem::path candidate = base /
            ("linuxscp-unittest-" + std::to_string(attempt) + "-" +
             std::to_string(static_cast<unsigned long>(time(NULL))));
        if (std::filesystem::create_directory(candidate, ec))
        {
#if defined(_WIN32)
            return candidate.u8string();
#else
            return candidate.string();
#endif
        }
        ec.clear();
    }
    return std::string();
}

bool RemoveDirectoryQuiet(const std::string & path)
{
    std::error_code ec;
#if defined(_WIN32)
    return std::filesystem::remove(std::filesystem::u8path(path), ec) && !ec;
#else
    return std::filesystem::remove(std::filesystem::path(path), ec) && !ec;
#endif
}

std::string Words(const std::vector<std::string> & words)
{
    std::string result;
    for (size_t i = 0; i < words.size(); i++)
    {
        result += "[" + words[i] + "]";
    }
    return result;
}

void TestSplitCommandLine()
{
    CHECK_EQ(Words(SplitCommandLine("get a.txt b.txt")), "[get][a.txt][b.txt]");
    CHECK_EQ(Words(SplitCommandLine("   put   /tmp/a   .   ")), "[put][/tmp/a][.]");
    CHECK_EQ(Words(SplitCommandLine("")), "");
    CHECK_EQ(Words(SplitCommandLine("get 'two words.txt' dest")),
        "[get][two words.txt][dest]");
    CHECK_EQ(Words(SplitCommandLine("get \"two words.txt\" dest")),
        "[get][two words.txt][dest]");
#if defined(_WIN32)
    /* A backslash is a path separator on Windows, never an escape:
     * C:\dir\file.txt must survive word splitting untouched. */
    CHECK_EQ(Words(SplitCommandLine("put C:\\dir\\file.txt .")),
        "[put][C:\\dir\\file.txt][.]");
    CHECK_EQ(Words(SplitCommandLine("get 'two words.txt' dest")),
        "[get][two words.txt][dest]");
#else
    CHECK_EQ(Words(SplitCommandLine("get a\\ b.txt")), "[get][a b.txt]");
#endif
    /* An unterminated quote still yields the word read so far. */
    CHECK_EQ(Words(SplitCommandLine("get 'unterminated")), "[get][unterminated]");
    /* An empty quoted argument is a word of its own. */
    CHECK_EQ(Words(SplitCommandLine("get \"\" x")), "[get][][x]");
}

void TestSplitCommands()
{
    CHECK_EQ(Words(SplitCommands("pwd")), "[pwd]");
    CHECK_EQ(Words(SplitCommands("open sftp://h/ ; pwd ; exit")),
        "[open sftp://h/ ][ pwd ][ exit]");
    CHECK_EQ(Words(SplitCommands("cd /a;cd /b")), "[cd /a][cd /b]");
    /* A trailing ';' does not produce an empty command. */
    CHECK_EQ(Words(SplitCommands("pwd;")), "[pwd]");
    CHECK_EQ(Words(SplitCommands("")), "");
    /* A ';' inside quotes is part of the argument. */
    CHECK_EQ(Words(SplitCommands("get 'a;b.txt' .")), "[get 'a;b.txt' .]");
    CHECK_EQ(Words(SplitCommands("get \"a;b.txt\" .")), "[get \"a;b.txt\" .]");
}

void TestRemotePaths()
{
    CHECK_TRUE(IsRemoteAbsolutePath("/tmp"));
    CHECK_TRUE(IsRemoteAbsolutePath("/"));
    CHECK_FALSE(IsRemoteAbsolutePath("tmp"));
    CHECK_FALSE(IsRemoteAbsolutePath(""));

    CHECK_EQ(RemoteDirName("/tmp/file.txt"), "/tmp");
    CHECK_EQ(RemoteDirName("/file.txt"), "/");
    CHECK_EQ(RemoteDirName("file.txt"), "");
    CHECK_EQ(RemoteFileName("/tmp/file.txt"), "file.txt");
    CHECK_EQ(RemoteFileName("file.txt"), "file.txt");
    CHECK_EQ(RemoteFileName("/tmp/"), "");

    CHECK_EQ(JoinRemotePath("/tmp", "file.txt"), "/tmp/file.txt");
    CHECK_EQ(JoinRemotePath("/tmp/", "file.txt"), "/tmp/file.txt");
    CHECK_EQ(JoinRemotePath("/tmp", "/etc/passwd"), "/etc/passwd");
    CHECK_EQ(JoinRemotePath("/tmp", ""), "/tmp");
    CHECK_EQ(JoinRemotePath("", "file.txt"), "file.txt");

    CHECK_EQ(ResolveRemotePath("/home/user", "sub"), "/home/user/sub");
    CHECK_EQ(ResolveRemotePath("/home/user", "./sub/../other"), "/home/user/other");
    CHECK_EQ(ResolveRemotePath("/home/user", ".."), "/home");
    CHECK_EQ(ResolveRemotePath("/home", ".."), "/");
    CHECK_EQ(ResolveRemotePath("/", ".."), "/");
    CHECK_EQ(ResolveRemotePath("/home/user", "/etc"), "/etc");
    CHECK_EQ(ResolveRemotePath("/home/user", "/"), "/");
    CHECK_EQ(ResolveRemotePath("", "sub"), "/sub");
    /* A trailing slash is not part of the resolved name. */
    CHECK_EQ(ResolveRemotePath("/home/user", "sub/"), "/home/user/sub");
}

void TestLocalPaths()
{
    std::string homeDir = HomeDirectory();
    if (!homeDir.empty())
    {
        CHECK_EQ(ExpandTilde("~"), homeDir);
        CHECK_EQ(ExpandTilde("~/docs"), JoinLocalPath(homeDir, "docs"));
        /* '~user' is deliberately not supported. */
        CHECK_EQ(ExpandTilde("~user"), "~user");
    }
    CHECK_EQ(ExpandTilde(""), "");
#if !defined(_WIN32)
    CHECK_EQ(ExpandTilde("/tmp"), "/tmp");
    const char * oldHomeValue = getenv("HOME");
    const bool hadHome = oldHomeValue != NULL;
    const std::string oldHome = hadHome ? oldHomeValue : "";
    SetEnvVar("HOME", "/");
    CHECK_EQ(ExpandTilde("~/docs"), "/docs");
    SetEnvVar("HOME", hadHome ? oldHome.c_str() : NULL);

    CHECK_EQ(LocalDirName("/tmp/file.txt"), "/tmp");
    CHECK_EQ(LocalDirName("file.txt"), ".");
    CHECK_EQ(LocalFileName("/tmp/file.txt"), "file.txt");
    CHECK_EQ(JoinLocalPath("/tmp", "a"), "/tmp/a");
    CHECK_EQ(JoinLocalPath("/tmp/", "a"), "/tmp/a");
    CHECK_EQ(JoinLocalPath("/tmp", "/etc/passwd"), "/etc/passwd");

    CHECK_EQ(ResolveLocalPath("/home/user", "sub"), "/home/user/sub");
    CHECK_EQ(ResolveLocalPath("/home/user", "/etc"), "/etc");
    CHECK_EQ(ResolveLocalPath("/home/user", "."), "/home/user");
    if (!homeDir.empty())
    {
        CHECK_EQ(ResolveLocalPath("/tmp", "~/docs"), JoinLocalPath(homeDir, "docs"));
    }
    CHECK_FALSE(IsLocalAbsolutePath("tmp/file"));
    CHECK_TRUE(IsLocalAbsolutePath("/tmp"));
#else
    /* Drive-letter, rooted and UNC paths are absolute; drive-relative
     * and plain relative paths are not. */
    CHECK_TRUE(IsLocalAbsolutePath("C:\\dir\\file.txt"));
    CHECK_TRUE(IsLocalAbsolutePath("C:/dir/file.txt"));
    CHECK_TRUE(IsLocalAbsolutePath("\\\\server\\share\\file.txt"));
    CHECK_TRUE(IsLocalAbsolutePath("\\dir\\file.txt"));
    CHECK_FALSE(IsLocalAbsolutePath("C:file.txt"));
    CHECK_FALSE(IsLocalAbsolutePath("dir\\file.txt"));

    CHECK_EQ(LocalDirName("C:\\tmp\\file.txt"), "C:\\tmp");
    CHECK_EQ(LocalDirName("C:\\file.txt"), "C:\\");
    CHECK_EQ(LocalDirName("C:/tmp/file.txt"), "C:/tmp");
    CHECK_EQ(LocalDirName("file.txt"), ".");
    CHECK_EQ(LocalFileName("C:\\tmp\\file.txt"), "file.txt");
    CHECK_EQ(LocalFileName("C:\\tmp\\"), "tmp");
    CHECK_EQ(JoinLocalPath("C:\\tmp", "a.txt"), "C:\\tmp\\a.txt");
    CHECK_EQ(JoinLocalPath("C:\\tmp\\", "a.txt"), "C:\\tmp\\a.txt");
    CHECK_EQ(JoinLocalPath("C:\\tmp", "D:\\other.txt"), "D:\\other.txt");
    CHECK_EQ(JoinLocalPath("C:\\tmp", "\\rooted.txt"), "C:\\rooted.txt");

    CHECK_EQ(ResolveLocalPath("C:\\home\\user", "sub"), "C:\\home\\user\\sub");
    CHECK_EQ(ResolveLocalPath("C:\\home\\user", "..\\other"), "C:\\home\\other");
    CHECK_EQ(ResolveLocalPath("C:\\home\\user", "D:\\etc"), "D:\\etc");
    CHECK_EQ(ResolveLocalPath("C:\\home\\user", "."), "C:\\home\\user");
    /* Forward slashes are accepted and normalised. */
    CHECK_EQ(ResolveLocalPath("C:/home/user", "sub"), "C:\\home\\user\\sub");

    /* '~' follows %USERPROFILE% on Windows. */
    const char * oldProfile = getenv("USERPROFILE");
    const bool hadProfile = oldProfile != NULL;
    const std::string savedProfile = hadProfile ? oldProfile : "";
    SetEnvVar("USERPROFILE", "C:\\Users\\test");
    CHECK_EQ(ExpandTilde("~"), "C:\\Users\\test");
    CHECK_EQ(ExpandTilde("~\\docs"), "C:\\Users\\test\\docs");
    CHECK_EQ(ExpandTilde("~/docs"), "C:\\Users\\test\\docs");
    SetEnvVar("USERPROFILE", hadProfile ? savedProfile.c_str() : NULL);
#endif
}

void TestMasks()
{
    CHECK_TRUE(HasWildcard("*.txt"));
    CHECK_TRUE(HasWildcard("a?.txt"));
    CHECK_FALSE(HasWildcard("a.txt"));
    CHECK_FALSE(HasWildcard(""));

    CHECK_TRUE(MatchesMask("*.txt", "a.txt"));
    CHECK_TRUE(MatchesMask("*.txt", ".txt"));
    CHECK_FALSE(MatchesMask("*.txt", "a.tar"));
    CHECK_TRUE(MatchesMask("a?c", "abc"));
    CHECK_FALSE(MatchesMask("a?c", "ac"));
    CHECK_FALSE(MatchesMask("a?c", "abbc"));
    CHECK_TRUE(MatchesMask("*", "anything"));
    CHECK_TRUE(MatchesMask("*", ""));
    CHECK_TRUE(MatchesMask("*.tar.gz", "linuxscp-1.0.tar.gz"));
    CHECK_TRUE(MatchesMask("a*b*c", "axxbyyc"));
    CHECK_FALSE(MatchesMask("a*b*c", "axxbyy"));
    /* A mask without wildcards matches exactly one name. */
    CHECK_TRUE(MatchesMask("file.txt", "file.txt"));
    CHECK_FALSE(MatchesMask("file.txt", "file.txtx"));
    CHECK_FALSE(MatchesMask("", "a"));
    CHECK_TRUE(MatchesMask("", ""));
}

void TestMaskCase()
{
    CHECK_TRUE(MatchesMaskInsensitive("*.TXT", "a.txt"));
    CHECK_TRUE(MatchesMaskInsensitive("*.txt", "A.TxT"));
    CHECK_FALSE(MatchesMaskInsensitive("*.txt", "a.tar"));
    CHECK_FALSE(MatchesMask("*.TXT", "a.txt"));
}

void TestPlatformServices()
{
    /* Process capture returns the exit code and both streams. */
    std::string out;
    std::string err;
#if defined(_WIN32)
    std::string cmd = FindExecutableOnPath("cmd");
    CHECK_TRUE(!cmd.empty());
    if (!cmd.empty())
    {
        std::vector<std::string> args;
        args.push_back(cmd);
        args.push_back("/c");
        args.push_back("echo hello-windows");
        CHECK_TRUE(RunCapture(args, out, err) == 0);
        CHECK_TRUE(out.find("hello-windows") != std::string::npos);
    }
#else
    std::vector<std::string> args;
    args.push_back("echo");
    args.push_back("hello-posix");
    CHECK_TRUE(RunCapture(args, out, err) == 0);
    CHECK_TRUE(out.find("hello-posix") != std::string::npos);
#endif
    /* A missing program is a start failure, not a crash. */
    std::vector<std::string> missing;
    missing.push_back("linuxscp-no-such-program-xyz");
    CHECK_TRUE(RunCapture(missing, out, err) == -1 || !err.empty() || out.empty());

    CHECK_TRUE(!HomeDirectory().empty() || !ConfigDirectory().empty());
}

void TestDateParsing()
{
    /*
     * The listing date is read in local time. Pin the zone so that the
     * expected values do not depend on where the tests run.
     */
#if defined(_WIN32)
    _putenv_s("TZ", "UTC");
    _tzset();
#else
    setenv("TZ", "UTC", 1);
    tzset();
#endif

    /* curl prints Last-Modified in GMT. */
    CHECK_EQ(std::to_string(ParseHttpDate("Tue, 13 Oct 2026 05:21:00 GMT")),
        "1791868860");
    CHECK_EQ(std::to_string(ParseHttpDate("Sun, 03 Mar 2019 12:00:00 GMT")),
        "1551614400");
    CHECK_EQ(std::to_string(ParseHttpDate("")), "0");
    CHECK_EQ(std::to_string(ParseHttpDate("not a date")), "0");

    /* An 'ls -l' date column: a year for older entries, a clock time
     * otherwise. */
    CHECK_EQ(std::to_string(ParseListingDate("Mar", "3", "2019")), "1551571200");
    CHECK_EQ(std::to_string(ParseListingDate("Oct", "13", "2026")),
        "1791849600");
    CHECK_EQ(std::to_string(ParseListingDate("Mar", "3", "25:00")), "0");
    CHECK_EQ(std::to_string(ParseListingDate("Mar", "3", "12:60")), "0");
    CHECK_EQ(std::to_string(ParseListingDate("Xyz", "3", "2019")), "0");
    CHECK_EQ(std::to_string(ParseListingDate("Mar", "32", "2019")), "0");
    CHECK_EQ(std::to_string(ParseListingDate("Mar", "", "2019")), "0");
    const unsigned long thisYear = ParseListingDate("Jan", "1", "00:00");
    CHECK_TRUE(thisYear >= 1767225600UL);
}

/* The 'terminal' command hands the connection details to the system ssh
 * client; the argument list must not grow options that ssh does not
 * take, and a PuTTY key file must not be passed as -i. */
void TestTerminalCommandLine()
{
    const std::vector<std::string> plain = BuildSshArguments("user", "host", 22, "");
    CHECK_EQ(std::to_string(plain.size()), "2");
    CHECK_EQ(plain[0], "-t");
    CHECK_EQ(plain[1], "user@host");

    const std::vector<std::string> full = BuildSshArguments("user", "host", 2222,
        "/home/user/.ssh/id_ed25519");
    CHECK_EQ(std::to_string(full.size()), "6");
    CHECK_EQ(full[0], "-i");
    CHECK_EQ(full[1], "/home/user/.ssh/id_ed25519");
    CHECK_EQ(full[2], "-p");
    CHECK_EQ(full[3], "2222");
    CHECK_EQ(full[4], "-t");
    CHECK_EQ(full[5], "user@host");

    /* No user name: the client asks for one itself. */
    const std::vector<std::string> anonymous = BuildSshArguments("", "host", 22, "");
    CHECK_EQ(anonymous[1], "host");

    /* A PuTTY key file cannot be read by the ssh client. */
    const std::vector<std::string> ppk = BuildSshArguments("user", "host", 22,
        "C:/keys/private.PPK");
    CHECK_EQ(std::to_string(ppk.size()), "2");
    CHECK_EQ(ppk[0], "-t");
}

void TestFormatting()
{
    CHECK_EQ(FormatSize(0), "0");
    CHECK_EQ(FormatSize(12345), "12345");
    CHECK_EQ(FormatSize(1073741824ULL), "1073741824");

    CHECK_EQ(FormatPermissions(0040755), "drwxr-xr-x");
    CHECK_EQ(FormatPermissions(0100644), "-rw-r--r--");
    CHECK_EQ(FormatPermissions(0100600), "-rw-------");
    CHECK_EQ(FormatPermissions(0104755), "-rwsr-xr-x");
    CHECK_EQ(FormatPermissions(0100755), "-rwxr-xr-x");
    CHECK_EQ(FormatPermissions(0041755), "drwxr-xr-t");
    CHECK_EQ(FormatPermissions(0041754), "drwxr-xr-T");

    /* The exact layout of a timestamp is locale and clock dependent, so
     * only check that it looks like 'ls -l' output. */
    const std::string when = FormatFileTime(1700000000UL);
    CHECK_TRUE(when.size() >= 11);
    CHECK_TRUE(when[3] == ' ');
    CHECK_EQ(FormatFileTime(0).substr(0, 3), "Jan");
}

void TestParseSiteUrl()
{
    SessionOptions options;
    std::string error;

    CHECK_TRUE(ParseSiteUrl("sftp://user:secret@example.com:2222/var/data",
        options, error));
    CHECK_EQ(options.hostName, "example.com");
    CHECK_TRUE(options.portNumber == 2222);
    CHECK_EQ(options.userName, "user");
    CHECK_EQ(options.password, "secret");
    CHECK_EQ(options.initialDirectory, "/var/data");

    /* Defaults. */
    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("sftp://example.com", options, error));
    CHECK_EQ(options.hostName, "example.com");
    CHECK_TRUE(options.portNumber == 22);
    CHECK_EQ(options.userName, "");
    CHECK_EQ(options.initialDirectory, "");

    /* Global command-line settings survive URLs that omit those fields. */
    options = SessionOptions();
    options.portNumber = 2200;
    options.userName = "default-user";
    options.password = "default-password";
    options.timeoutSecs = 30;
    options.privateKeyFile = "/tmp/client.ppk";
    options.hostKey = "SHA256:pinned";
    options.verbose = true;
    CHECK_TRUE(ParseSiteUrl("example.com", options, error));
    CHECK_TRUE(options.portNumber == 2200);
    CHECK_EQ(options.userName, "default-user");
    CHECK_EQ(options.password, "default-password");
    CHECK_TRUE(options.timeoutSecs == 30);
    CHECK_EQ(options.privateKeyFile, "/tmp/client.ppk");
    CHECK_EQ(options.hostKey, "SHA256:pinned");
    CHECK_TRUE(options.verbose);

    /* URI-escaped credentials and remote path are decoded. */
    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl(
        "sftp://url%40user:p%40ss%3Aword@example.com:2200/a%20b", options, error));
    CHECK_EQ(options.userName, "url@user");
    CHECK_EQ(options.password, "p@ss:word");
    CHECK_TRUE(options.portNumber == 2200);
    CHECK_EQ(options.initialDirectory, "/a b");

    /* No scheme at all. */
    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("example.com:2222/tmp", options, error));
    CHECK_EQ(options.hostName, "example.com");
    CHECK_TRUE(options.portNumber == 2222);
    CHECK_EQ(options.initialDirectory, "/tmp");

    /* The 'ssh' and 'scp' aliases of SFTP. */
    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("ssh://example.com", options, error));
    CHECK_EQ(options.hostName, "example.com");

    /* An IPv6 literal in brackets. */
    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("sftp://[::1]:2222/tmp", options, error));
    CHECK_EQ(options.hostName, "::1");
    CHECK_TRUE(options.portNumber == 2222);
    CHECK_EQ(options.initialDirectory, "/tmp");

    /* Multi-protocol URLs. */
    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("ftp://example.com", options, error));
    CHECK_EQ(options.protocol, "ftp");
    CHECK_EQ(options.hostName, "example.com");
    CHECK_TRUE(options.portNumber == 21);

    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("ftps://example.com", options, error));
    CHECK_EQ(options.protocol, "ftps");
    CHECK_TRUE(options.portNumber == 990);

    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("webdav://example.com/dav", options, error));
    CHECK_EQ(options.protocol, "webdav");
    CHECK_EQ(options.initialDirectory, "/dav");

    options = SessionOptions();
    CHECK_TRUE(ParseSiteUrl("s3://bucket.example.com", options, error));
    CHECK_EQ(options.protocol, "s3");

    /* Rejected URLs. */
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("gopher://example.com", options, error));
    CHECK_TRUE(error.find("Unsupported protocol") != std::string::npos);
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://", options, error));
    CHECK_TRUE(error.find("No host name") != std::string::npos);
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://example.com:0", options, error));
    CHECK_TRUE(error.find("Invalid port") != std::string::npos);
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://[::1", options, error));
    CHECK_TRUE(error.find("Malformed") != std::string::npos);
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://[::1]junk", options, error));
    CHECK_TRUE(error.find("Invalid port") != std::string::npos);
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://example.com:22junk", options, error));
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://[::1]:2222x", options, error));
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://example.com/%XZ", options, error));
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://user:secret@example.com:22junk", options, error));
    CHECK_TRUE(error.find("secret") == std::string::npos);
    options = SessionOptions();
    CHECK_FALSE(ParseSiteUrl("sftp://::1", options, error));
    CHECK_TRUE(error.find("brackets") != std::string::npos);
}

void TestLocalFs()
{
    const std::string dir = MakeTempDirectory();
    CHECK_TRUE(!dir.empty());
    if (dir.empty())
    {
        return;
    }
    std::string error;

    CHECK_TRUE(CreateLocalDirectory(dir, error));
    CHECK_TRUE(error.empty());
    CHECK_TRUE(CreateLocalDirectory(dir + "/sub", error));

    /* Binary mode: the size check below is about six bytes, and a text
     * stream would turn the newline into CRLF on Windows. */
    FILE * file = fopen((dir + "/one.txt").c_str(), "wb");
    CHECK_TRUE(file != NULL);
    if (file != NULL)
    {
        fputs("hello\n", file);
        fclose(file);
    }
    file = fopen((dir + "/two.log").c_str(), "wb");
    CHECK_TRUE(file != NULL);
    if (file != NULL)
    {
        fclose(file);
    }

    /* EEXIST is success only when the existing object is a directory. */
    CHECK_FALSE(CreateLocalDirectory(dir + "/one.txt", error));
    CHECK_TRUE(!error.empty());

    LocalFileInfo info = StatLocal(dir + "/one.txt");
    CHECK_TRUE(info.exists);
    CHECK_TRUE(info.error == 0);
    CHECK_FALSE(info.isDir);
    CHECK_TRUE(info.isRegular);
    CHECK_TRUE(info.size == 6);
    CHECK_TRUE(info.mtime > 0);
    info = StatLocal(dir + "/sub");
    CHECK_TRUE(info.exists);
    CHECK_TRUE(info.isDir);
    CHECK_FALSE(info.isRegular);
    info = StatLocal(dir + "/missing");
    CHECK_FALSE(info.exists);
    CHECK_TRUE(info.error == ENOENT);

    std::vector<std::string> names;
    CHECK_TRUE(ListLocalDirectory(dir, names, error));
    CHECK_TRUE(error.empty());
    CHECK_TRUE(names.size() == 3);
    CHECK_FALSE(ListLocalDirectory(dir + "/missing", names, error));
    CHECK_TRUE(!error.empty());
    CHECK_TRUE(ListLocalDirectory(dir, names, error));
    CHECK_TRUE(error.empty());

    names.clear();
    CHECK_TRUE(ExpandLocalWildcard(dir + "/*.txt", names, error));
    CHECK_TRUE(names.size() == 1);
    if (!names.empty())
    {
        CHECK_EQ(names[0], JoinLocalPath(dir, "one.txt"));
    }
    names.clear();
    CHECK_TRUE(ExpandLocalWildcard(dir + "/*", names, error));
    CHECK_TRUE(names.size() == 3);
    names.clear();
    /* A mask that matches nothing is not an error, it is just empty. */
    CHECK_TRUE(ExpandLocalWildcard(dir + "/*.none", names, error));
    CHECK_TRUE(names.empty());
    /* A plain name resolves to itself. */
    names.clear();
    CHECK_TRUE(ExpandLocalWildcard(dir + "/one.txt", names, error));
    CHECK_TRUE(names.size() == 1);

    /* The current local directory is restored, even after failed chdir. */
    const std::string originalDirectory = GetLocalDirectory(error);
    CHECK_TRUE(!originalDirectory.empty());
    CHECK_TRUE(SetLocalDirectory(dir, error));
    CHECK_EQ(GetLocalDirectory(error), dir);
    CHECK_FALSE(SetLocalDirectory(dir + "/missing", error));
    CHECK_TRUE(!error.empty());
    CHECK_TRUE(SetLocalDirectory(originalDirectory, error));

    CHECK_TRUE(RemoveLocalFile(dir + "/one.txt", error));
    CHECK_TRUE(RemoveLocalFile(dir + "/two.log", error));
    CHECK_EQ(error, "");
    CHECK_TRUE(RemoveDirectoryQuiet(dir + "/sub"));

    /* Recursive directory creation handles multiple missing parent directories. */
    CHECK_TRUE(CreateLocalDirectoryRecursive(dir + "/deep/nested/path", error));
    CHECK_TRUE(error.empty());
    LocalFileInfo deepInfo = StatLocal(dir + "/deep/nested/path");
    CHECK_TRUE(deepInfo.exists);
    CHECK_TRUE(deepInfo.isDir);
    CHECK_TRUE(RemoveDirectoryQuiet(dir + "/deep/nested/path"));
    CHECK_TRUE(RemoveDirectoryQuiet(dir + "/deep/nested"));
    CHECK_TRUE(RemoveDirectoryQuiet(dir + "/deep"));

    CHECK_TRUE(RemoveDirectoryQuiet(dir));
}

void TestScriptCommands()
{
    Script script;
    CHECK_TRUE(script.ExecuteLine("echo test message"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.ExecuteLine("option confirm off"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.ExecuteLine("option resume on"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.ExecuteLine("option resume off"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.ExecuteLine("option batch abort"));
    CHECK_TRUE(script.ErrorCount() == 0);

    CHECK_TRUE(script.ExecuteLine("option transfer binary"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.ExecuteLine("binary"));
    CHECK_TRUE(script.ExecuteLine("option batch continue"));
    /* 'reconnect' (without the 'time') stays unsupported; 'reconnecttime'
     * is a real option now. */
    CHECK_TRUE(script.ExecuteLine("option reconnect 10"));
    CHECK_TRUE(script.ErrorCount() == 1);
    CHECK_TRUE(script.ExecuteLine("option transfer ascii"));
    CHECK_TRUE(script.ErrorCount() == 1);
    CHECK_TRUE(script.ExecuteLine("option transfer"));
    CHECK_TRUE(script.ExecuteLine("option transfer automatic"));
    CHECK_TRUE(script.ExecuteLine("option transfer"));
    CHECK_TRUE(script.ExecuteLine("ascii"));
    CHECK_TRUE(script.ExecuteLine("binary"));
    /* include/exclude are one WinSCP mask: setting one clears the other. */
    CHECK_TRUE(script.ExecuteLine("option include *.txt"));
    CHECK_TRUE(script.ExecuteLine("option exclude *.bak;*.tmp"));
    CHECK_TRUE(script.ExecuteLine("option failonnomatch on"));
    CHECK_TRUE(script.ExecuteLine("option failonnomatch"));
    CHECK_TRUE(script.ExecuteLine("option echo on"));
    CHECK_TRUE(script.ExecuteLine("option synchdelete off"));
    CHECK_TRUE(script.ExecuteLine("option reconnecttime 30"));
    CHECK_TRUE(script.ExecuteLine("option reconnecttime off"));
    CHECK_TRUE(script.ExecuteLine("option reconnecttime nope"));
    CHECK_TRUE(script.ErrorCount() == 2);
    CHECK_FALSE(script.ExecuteLine("exit"));
    CHECK_TRUE(script.ExitRequested());

    Script aborting;
    CHECK_TRUE(aborting.ExecuteLine("option batch abort"));
    CHECK_FALSE(aborting.ExecuteLine("invalid-command"));
    CHECK_TRUE(aborting.ExitRequested());
    CHECK_TRUE(aborting.ErrorCount() == 1);
    CHECK_FALSE(aborting.ExecuteLine("option confirm off"));
    CHECK_TRUE(aborting.ErrorCount() == 1);
    Console::Instance().SetBatchMode(false);
}

void TestSessionRegistry()
{
    /* The SFTP transport glue in PuttyGlue.cpp reaches the live session
     * through this pair, so it must start out empty and follow
     * SetCurrentSession(). */
    CHECK_TRUE(CurrentSession() == NULL);
    Session session;
    CHECK_FALSE(session.IsOpen());
    CHECK_FALSE(session.IsAlive());
    SetCurrentSession(&session);
    CHECK_TRUE(CurrentSession() == &session);
    SetCurrentSession(NULL);
    CHECK_TRUE(CurrentSession() == NULL);
}

/* ------------------------------------------------------------------ *
 * Synchronization planning                                            *
 * ------------------------------------------------------------------ */

namespace {

/* An in-memory tree, so that the planner can be tested without a
 * server and without touching the file system. Paths are kept with '/'
 * on both sides and normalised on the way in, so that the same tree
 * works for the local side (whose separator is the host one) and for
 * the remote side (always POSIX). */
class MemoryFileSystem : public SyncFileSystem
{
public:
    MemoryFileSystem(const std::string & label, const std::string & root) :
        mLabel(label), mRoot(Normalize(root))
    {
        mDirectories[mRoot];
    }

    void AddFile(const std::string & path, unsigned long long size,
        unsigned long mtime)
    {
        Add(path, false, size, mtime);
    }

    void AddDirectory(const std::string & path, unsigned long mtime)
    {
        Add(path, true, 0, mtime);
    }

    /* An existing but empty root, as opposed to a missing one. */
    void MakeRootListable() { mDirectories[mRoot]; }

    virtual bool ListDirectory(const std::string & path,
        std::vector<SyncEntry> & entries, std::string & error)
    {
        std::string key = Normalize(path);
        while (key.size() > 1 && key[key.size() - 1] == '/')
        {
            key.erase(key.size() - 1);
        }
        std::map<std::string, std::vector<SyncEntry> >::const_iterator it =
            mDirectories.find(key);
        if (it == mDirectories.end())
        {
            error = "no such directory";
            return false;
        }
        entries = it->second;
        return true;
    }

    virtual const char * Label() const { return mLabel.c_str(); }

private:
    static std::string Normalize(const std::string & path)
    {
        std::string result = path;
        for (size_t i = 0; i < result.size(); i++)
        {
            if (result[i] == '\\')
            {
                result[i] = '/';
            }
        }
        return result;
    }

    bool HasEntry(const std::string & directory, const std::string & name) const
    {
        std::map<std::string, std::vector<SyncEntry> >::const_iterator it =
            mDirectories.find(directory);
        if (it == mDirectories.end())
        {
            return false;
        }
        for (size_t i = 0; i < it->second.size(); i++)
        {
            if (it->second[i].name == name)
            {
                return true;
            }
        }
        return false;
    }

    void Add(const std::string & path, bool isDirectory, unsigned long long size,
        unsigned long mtime)
    {
        const std::vector<std::string> parts = SplitRelativePath(Normalize(path));
        if (parts.empty())
        {
            return;
        }
        std::string walk = mRoot;
        for (size_t i = 0; i + 1 < parts.size(); i++)
        {
            if (!HasEntry(walk, parts[i]))
            {
                SyncEntry parent;
                parent.name = parts[i];
                parent.isDirectory = true;
                mDirectories[walk].push_back(parent);
            }
            walk += "/" + parts[i];
            mDirectories[walk];
        }
        SyncEntry entry;
        entry.name = parts.back();
        entry.isDirectory = isDirectory;
        entry.size = size;
        entry.mtime = mtime;
        if (!HasEntry(walk, entry.name))
        {
            mDirectories[walk].push_back(entry);
        }
        if (isDirectory)
        {
            mDirectories[walk + "/" + entry.name];
        }
    }

    std::map<std::string, std::vector<SyncEntry> > mDirectories;
    std::string mLabel;
    std::string mRoot;
};

const SyncItem * FindItem(const SyncPlan & plan, const std::string & path)
{
    for (size_t i = 0; i < plan.items.size(); i++)
    {
        if (plan.items[i].path == path)
        {
            return &plan.items[i];
        }
    }
    return NULL;
}

SyncAction ActionOf(const SyncPlan & plan, const std::string & path)
{
    const SyncItem * item = FindItem(plan, path);
    return item != NULL ? item->action : SyncAction::None;
}

bool RemoveTreeQuiet(const std::string & path)
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path), ec);
    return !ec;
}

} // namespace

void TestFileMaskParsing()
{
    FileMask mask;
    std::string error;

    CHECK_TRUE(mask.Parse("*.txt", error));
    CHECK_TRUE(mask.include.size() == 1);
    CHECK_TRUE(mask.exclude.empty());
    CHECK_TRUE(mask.Accepts("a.txt", false));
    CHECK_FALSE(mask.Accepts("a.log", false));
    /* An include mask never prunes a directory. */
    CHECK_TRUE(mask.Accepts("logs", true));

    CHECK_TRUE(mask.Parse("*.txt|*.bak", error));
    CHECK_TRUE(mask.include.size() == 1);
    CHECK_TRUE(mask.exclude.size() == 1);
    CHECK_TRUE(mask.Accepts("a.txt", false));
    CHECK_FALSE(mask.Accepts("a.bak", false));
    CHECK_TRUE(mask.Accepts("logs", true));
    CHECK_FALSE(mask.Accepts("cache.bak", true));

    CHECK_TRUE(mask.Parse("|*.bak;*.tmp", error));
    CHECK_TRUE(mask.include.empty());
    CHECK_TRUE(mask.exclude.size() == 2);
    CHECK_TRUE(mask.Accepts("anything", false));
    CHECK_FALSE(mask.Accepts("a.bak", false));
    CHECK_FALSE(mask.Accepts("a.tmp", false));
    /* Exclusions do prune a directory. */
    CHECK_FALSE(mask.Accepts("cache.tmp", true));

    CHECK_TRUE(mask.Parse("*.txt;*.md", error));
    CHECK_TRUE(mask.include.size() == 2);
    CHECK_TRUE(mask.Accepts("readme.md", false));
    CHECK_FALSE(mask.Accepts("a.log", false));

    /* An empty mask selects everything. */
    CHECK_TRUE(mask.Parse("", error));
    CHECK_TRUE(mask.empty());
    CHECK_EQ(mask.Describe(), "(none)");

    /* Directory masks end with '/': they match directories only. */
    CHECK_TRUE(mask.Parse("*/build/", error));
    CHECK_FALSE(mask.Accepts("build", false));
    CHECK_TRUE(mask.Accepts("build", true));
    CHECK_FALSE(mask.Accepts("docs", true));

    /* Path masks match the path of a file below the transfer root;
     * they do not address directories (use a trailing '/' for that). */
    CHECK_TRUE(mask.Parse("sub/*.txt", error));
    CHECK_FALSE(mask.Accepts("a.txt", false, 0, 0, "a.txt"));
    CHECK_FALSE(mask.Accepts("a.txt", false, 0, 0, "other/a.txt"));
    CHECK_TRUE(mask.Accepts("a.txt", false, 0, 0, "sub/a.txt"));
    CHECK_FALSE(mask.Accepts("sub", true, 0, 0, "sub"));

    /* Exclusion by directory mask prunes a subtree at any depth. */
    CHECK_TRUE(mask.Parse("|*/node_modules/;*.tmp", error));
    CHECK_TRUE(mask.Accepts("app", true, 0, 0, "app"));
    CHECK_FALSE(mask.Accepts("app", true, 0, 0, "node_modules"));
    CHECK_FALSE(mask.Accepts("app", true, 0, 0, "web/node_modules"));
    CHECK_FALSE(mask.Accepts("a.tmp", false, 0, 0, "a.tmp"));

    /* Size criteria: 1K is 1024 bytes, 1M is 1024*1024. */
    CHECK_TRUE(mask.Parse("*.bin>1K", error));
    CHECK_TRUE(mask.Accepts("a.bin", false, 1025, 0, "a.bin"));
    CHECK_FALSE(mask.Accepts("a.bin", false, 1024, 0, "a.bin"));
    CHECK_FALSE(mask.Accepts("a.bin", false, 0, 0, "a.bin"));
    /* A mask with a size criterion never selects a directory (an
     * include mask still never prunes one). */
    CHECK_TRUE(mask.Accepts("a.bin", true, 5000000, 0, "a.bin"));
    CHECK_TRUE(mask.Parse("|*.bin>1K", error));
    CHECK_FALSE(mask.Accepts("a.bin", false, 5000000, 0, "a.bin"));
    CHECK_TRUE(mask.Accepts("a.bin", true, 5000000, 0, "a.bin"));

    CHECK_TRUE(mask.Parse("*.bin<=1M", error));
    CHECK_TRUE(mask.Accepts("a.bin", false, 1024 * 1024, 0, "a.bin"));
    CHECK_FALSE(mask.Accepts("a.bin", false, 1024 * 1024 + 1, 0, "a.bin"));

    /* Time criteria are local dates, optionally with a time of day. */
    CHECK_TRUE(mask.Parse("*.log>=2020-06-01", error));
    const unsigned long juneFirst2020 = 1590969600UL; /* 2020-06-01 UTC */
    CHECK_FALSE(mask.Accepts("a.log", false, 10, juneFirst2020 - 86400, "a.log"));
    CHECK_TRUE(mask.Accepts("a.log", false, 10, juneFirst2020 + 86400, "a.log"));
    CHECK_TRUE(mask.Accepts("a.log", false, 10, juneFirst2020, "a.log"));
    CHECK_FALSE(mask.Accepts("a.log", false, 10, 0, "a.log"));

    /* Criteria chain within one token. */
    CHECK_TRUE(mask.Parse("*.dat>1K<10K", error));
    CHECK_TRUE(mask.Accepts("a.dat", false, 4096, 0, "a.dat"));
    CHECK_FALSE(mask.Accepts("a.dat", false, 20000, 0, "a.dat"));
    CHECK_FALSE(mask.Accepts("a.dat", false, 100, 0, "a.dat"));

    /* A token with only a criterion means '*<criterion>'. */
    CHECK_TRUE(mask.Parse(">2K", error));
    CHECK_TRUE(mask.Accepts("anything", false, 4096, 0, "anything"));
    CHECK_FALSE(mask.Accepts("anything", false, 100, 0, "anything"));

    /* Invalid criteria fail explicitly. */
    CHECK_FALSE(mask.Parse("*.txt>1X", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(mask.Parse("*.txt>notadate", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(mask.Parse("*.txt>=2026-13-40", error));
    CHECK_FALSE(error.empty());
    CHECK_EQ(mask.Describe(), "(none)");
}

void TestSyncPlannerModes()
{
    MemoryFileSystem local("local", "/local");
    MemoryFileSystem remote("remote", "/remote");
    std::string error;

    /* same.txt:        identical on both sides
     * older.txt:       the remote copy is newer
     * only-remote.txt: only on the remote side
     * only-local.txt:  only on the local side */
    local.AddFile("same.txt", 10, 1000);
    remote.AddFile("same.txt", 10, 1000);
    local.AddFile("older.txt", 10, 1000);
    remote.AddFile("older.txt", 20, 2000);
    remote.AddFile("only-remote.txt", 5, 1500);
    local.AddFile("only-local.txt", 7, 1500);

    SyncOptions options;
    SyncPlan plan;

    /* Local: the remote tree wins. */
    options.mode = SyncMode::Local;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_EQ(error, "");
    CHECK_TRUE(ActionOf(plan, "same.txt") == SyncAction::None);
    CHECK_TRUE(ActionOf(plan, "older.txt") == SyncAction::Download);
    CHECK_TRUE(ActionOf(plan, "only-remote.txt") == SyncAction::Download);
    CHECK_TRUE(ActionOf(plan, "only-local.txt") == SyncAction::None);
    CHECK_FALSE(plan.DeletesAnything());

    /* -delete removes what the winning side does not have. */
    options.deleteObsolete = true;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "only-local.txt") == SyncAction::DeleteLocal);
    CHECK_TRUE(plan.DeletesAnything());
    options.deleteObsolete = false;

    /* Remote: the local tree wins. */
    options.mode = SyncMode::Remote;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "older.txt") == SyncAction::None);
    CHECK_TRUE(ActionOf(plan, "only-local.txt") == SyncAction::Upload);
    CHECK_TRUE(ActionOf(plan, "only-remote.txt") == SyncAction::None);
    options.deleteObsolete = true;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "only-remote.txt") == SyncAction::DeleteRemote);
    options.deleteObsolete = false;

    /* Both, without a previous state: the newer side wins. */
    options.mode = SyncMode::Both;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "older.txt") == SyncAction::Download);
    CHECK_TRUE(ActionOf(plan, "only-remote.txt") == SyncAction::Download);
    CHECK_TRUE(ActionOf(plan, "only-local.txt") == SyncAction::Upload);
    CHECK_TRUE(ActionOf(plan, "same.txt") == SyncAction::None);
    /* Without a state nothing is deleted, whatever the switches say. */
    options.deleteObsolete = true;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_FALSE(plan.DeletesAnything());
    options.deleteObsolete = false;
}

void TestSyncPlannerCriteria()
{
    MemoryFileSystem local("local", "/local");
    MemoryFileSystem remote("remote", "/remote");
    std::string error;

    /* The remote file is older but larger. */
    local.AddFile("file.txt", 10, 2000);
    remote.AddFile("file.txt", 30, 1000);

    SyncOptions options;
    options.mode = SyncMode::Local;
    SyncPlan plan;

    options.criteria = SyncCriteria::Time;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "file.txt") == SyncAction::None);

    options.criteria = SyncCriteria::Size;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "file.txt") == SyncAction::Download);

    options.criteria = SyncCriteria::Either;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "file.txt") == SyncAction::Download);

    options.criteria = SyncCriteria::None;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "file.txt") == SyncAction::Download);
}

void TestSyncPlannerMaskAndRecursion()
{
    MemoryFileSystem local("local", "/local");
    MemoryFileSystem remote("remote", "/remote");
    std::string error;

    remote.AddDirectory("sub", 1000);
    remote.AddFile("sub/keep.txt", 4, 1200);
    remote.AddFile("sub/skip.log", 4, 1200);
    remote.AddFile("top.txt", 2, 1200);

    SyncOptions options;
    options.mode = SyncMode::Local;
    SyncPlan plan;
    CHECK_TRUE(options.mask.Parse("*.txt", error));

    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "sub/keep.txt") == SyncAction::Download);
    CHECK_FALSE(FindItem(plan, "sub/skip.log") != NULL);
    CHECK_TRUE(ActionOf(plan, "top.txt") == SyncAction::Download);

    /* Non-recursive: only the top directory is compared. */
    options.recursive = false;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(ActionOf(plan, "top.txt") == SyncAction::Download);
    CHECK_FALSE(FindItem(plan, "sub/keep.txt") != NULL);

    /* A directory that only one side has is planned as a whole subtree. */
    options.recursive = true;
    options.mask = FileMask();
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    const SyncItem * sub = FindItem(plan, "sub");
    CHECK_TRUE(sub != NULL && sub->isDirectory);
    CHECK_TRUE(sub != NULL && sub->action == SyncAction::Download);
    CHECK_TRUE(ActionOf(plan, "sub/skip.log") == SyncAction::Download);

    /* Plan paths are rebuilt with the separator of each side. */
    CHECK_EQ(LocalPathForRelative("/local", "sub/keep.txt"),
        JoinLocalPath(JoinLocalPath("/local", "sub"), "keep.txt"));
    CHECK_EQ(RemotePathForRelative("/remote", "sub/keep.txt"), "/remote/sub/keep.txt");
    CHECK_EQ(LocalPathForRelative("/local", ""), "/local");
}

void TestSyncPlannerConflictsAndState()
{
    MemoryFileSystem local("local", "/local");
    MemoryFileSystem remote("remote", "/remote");
    std::string error;

    local.AddFile("recorded.txt", 10, 1000);
    remote.AddFile("recorded.txt", 10, 1000);
    local.AddFile("deleted-on-remote.txt", 3, 1000);
    local.AddFile("new-locally.txt", 4, 1000);
    remote.AddFile("new-remotely.txt", 5, 1000);

    /* The state of the last synchronization: everything was equal except
     * for the file that has since been deleted on the remote side. */
    SyncState state;
    SyncStateEntry recorded;
    recorded.localTime = 1000;
    recorded.remoteTime = 1000;
    recorded.size = 10;
    state.Update("recorded.txt", recorded);
    recorded.size = 3;
    state.Update("deleted-on-remote.txt", recorded);

    SyncOptions options;
    options.mode = SyncMode::Both;
    options.deleteObsolete = true;
    options.state = &state;
    SyncPlan plan;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));

    /* Known on both sides and unchanged since: nothing to do. */
    CHECK_TRUE(ActionOf(plan, "recorded.txt") == SyncAction::None);
    /* Unknown to the state: new on one side, so it is copied. */
    CHECK_TRUE(ActionOf(plan, "new-locally.txt") == SyncAction::Upload);
    CHECK_TRUE(ActionOf(plan, "new-remotely.txt") == SyncAction::Download);
    /* It was on the remote side at the last pass and is gone now, so the
     * deletion is carried over to the local side instead of being read
     * as a new local file. */
    CHECK_TRUE(ActionOf(plan, "deleted-on-remote.txt") == SyncAction::DeleteLocal);

    /* Both sides changed since the last pass: a conflict, never a guess. */
    MemoryFileSystem local2("local", "/local");
    MemoryFileSystem remote2("remote", "/remote");
    local2.AddFile("recorded.txt", 11, 2000);
    remote2.AddFile("recorded.txt", 12, 3000);
    SyncPlan conflictPlan;
    CHECK_TRUE(PlanSynchronization(local2, remote2, "/local", "/remote",
        options, conflictPlan, error));
    CHECK_TRUE(ActionOf(conflictPlan, "recorded.txt") == SyncAction::Conflict);
    CHECK_TRUE(conflictPlan.HasConflicts());
    /* A plan with conflicts never deletes anything. */
    CHECK_FALSE(conflictPlan.DeletesAnything());

    /* One side changed: that side wins. */
    MemoryFileSystem local3("local", "/local");
    MemoryFileSystem remote3("remote", "/remote");
    local3.AddFile("recorded.txt", 11, 2000);
    remote3.AddFile("recorded.txt", 10, 1000);
    SyncPlan localWins;
    CHECK_TRUE(PlanSynchronization(local3, remote3, "/local", "/remote",
        options, localWins, error));
    CHECK_TRUE(ActionOf(localWins, "recorded.txt") == SyncAction::Upload);

    MemoryFileSystem local4("local", "/local");
    MemoryFileSystem remote4("remote", "/remote");
    local4.AddFile("recorded.txt", 10, 1000);
    remote4.AddFile("recorded.txt", 13, 4000);
    SyncPlan remoteWins;
    CHECK_TRUE(PlanSynchronization(local4, remote4, "/local", "/remote",
        options, remoteWins, error));
    CHECK_TRUE(ActionOf(remoteWins, "recorded.txt") == SyncAction::Download);
}

void TestSyncPlanOrder()
{
    /* Transfers are applied before deletions, and deletions deepest
     * first, so that a directory is empty by the time it is removed. */
    MemoryFileSystem local("local", "/local");
    MemoryFileSystem remote("remote", "/remote");
    local.MakeRootListable();
    local.AddDirectory("sub", 1000);
    local.AddFile("keep.txt", 10, 5000);
    remote.AddDirectory("sub", 1000);
    remote.AddFile("keep.txt", 10, 1000);
    remote.AddFile("old.txt", 2, 1);
    remote.AddFile("sub/old.txt", 2, 1);
    std::string error;

    SyncOptions options;
    options.mode = SyncMode::Remote;
    options.deleteObsolete = true;
    SyncPlan plan;
    CHECK_TRUE(PlanSynchronization(local, remote, "/local", "/remote",
        options, plan, error));
    CHECK_TRUE(plan.items.size() == 3);
    if (plan.items.size() == 3)
    {
        CHECK_EQ(plan.items[0].path, "keep.txt");
        CHECK_TRUE(plan.items[0].action == SyncAction::Upload);
        CHECK_TRUE(plan.items[1].action == SyncAction::DeleteRemote);
        CHECK_EQ(plan.items[1].path, "sub/old.txt");
        CHECK_TRUE(plan.items[2].action == SyncAction::DeleteRemote);
        CHECK_EQ(plan.items[2].path, "old.txt");
    }

    /* A whole directory that only one side has is removed in one item,
     * not one per file. */
    MemoryFileSystem emptyLocal("local", "/local");
    MemoryFileSystem onlyRemote("remote", "/remote");
    emptyLocal.MakeRootListable();
    onlyRemote.AddDirectory("gone", 1);
    onlyRemote.AddFile("gone/inside.txt", 2, 1);
    SyncPlan treePlan;
    CHECK_TRUE(PlanSynchronization(emptyLocal, onlyRemote, "/local", "/remote",
        options, treePlan, error));
    CHECK_TRUE(treePlan.items.size() == 1);
    CHECK_TRUE(treePlan.items.size() == 1 && treePlan.items[0].path == "gone");
    CHECK_TRUE(treePlan.items.size() == 1 && treePlan.items[0].isDirectory);
}

void TestSyncStateFile()
{
    const std::string temporary = MakeTempDirectory();
    if (temporary.empty())
    {
        return;
    }
    const char * oldConfig = getenv("LINUXSCP_CONFIG_DIR");
    const bool hadConfig = oldConfig != NULL;
    const std::string savedConfig = hadConfig ? oldConfig : "";
    SetEnvVar("LINUXSCP_CONFIG_DIR", temporary.c_str());

    const std::string key = SyncState::MakeKey("host", "/local", "/remote");
    CHECK_FALSE(key.empty());
    CHECK_TRUE(SyncState::MakeKey("host", "/local", "/remote") == key);
    CHECK_FALSE(SyncState::MakeKey("other", "/local", "/remote") == key);

    const std::string path = SyncState::DefaultPath(key);
    CHECK_FALSE(path.empty());

    SyncState state;
    SyncStateEntry entry;
    entry.localTime = 111;
    entry.remoteTime = 222;
    entry.size = 4096;
    state.Update("dir/file name.txt", entry);
    entry.localTime = 333;
    state.Update("plain.txt", entry);
    std::string error;
    CHECK_TRUE(state.Save(path, error));
    CHECK_TRUE(state.Size() == 2);

    SyncState reloaded;
    CHECK_TRUE(reloaded.Load(path, error));
    CHECK_TRUE(reloaded.Size() == 2);
    SyncStateEntry back;
    CHECK_TRUE(reloaded.Lookup("dir/file name.txt", back));
    CHECK_TRUE(back.localTime == 111);
    CHECK_TRUE(back.remoteTime == 222);
    CHECK_TRUE(back.size == 4096);
    CHECK_FALSE(reloaded.Lookup("missing", back));

    reloaded.Remove("plain.txt");
    CHECK_TRUE(reloaded.Size() == 1);
    CHECK_TRUE(reloaded.Save(path, error));

    SyncState afterRemoval;
    CHECK_TRUE(afterRemoval.Load(path, error));
    CHECK_TRUE(afterRemoval.Size() == 1);
    CHECK_FALSE(afterRemoval.Lookup("plain.txt", back));

    /* A missing state file is an empty state, not an error. */
    SyncState missingState;
    CHECK_TRUE(missingState.Load(path + ".does-not-exist", error));
    CHECK_TRUE(missingState.Empty());

    SetEnvVar("LINUXSCP_CONFIG_DIR", hadConfig ? savedConfig.c_str() : NULL);
    RemoveTreeQuiet(temporary);
}

/* ------------------------------------------------------------------ *
 * Stored sessions                                                     *
 * ------------------------------------------------------------------ */

void TestSessionStore()
{
    const std::string temporary = MakeTempDirectory();
    if (temporary.empty())
    {
        return;
    }
    const std::string path = temporary + "/sessions.ini";

    {
        SessionStore store(path);
        CHECK_TRUE(store.Empty());

        StoredSession session;
        session.name = "Servers/web.example.com";
        session.protocol = "sftp";
        session.hostName = "web.example.com";
        session.portNumber = 2222;
        session.userName = "alice";
        session.hostKey = "SHA256:abc";
        session.remoteDirectory = "/srv/www";
        session.localDirectory = "/home/alice/www";
        session.privateKeyFile = "/home/alice/.ssh/id_ed25519";
        std::string error;
        CHECK_TRUE(store.AddOrUpdate(session, error));
        CHECK_TRUE(store.Save(error));
        CHECK_TRUE(!store.Empty());
    }
    {
        SessionStore store(path);
        CHECK_TRUE(store.Sessions().size() == 1);
        const StoredSession * found = store.Find("Servers/web.example.com");
        CHECK_TRUE(found != NULL);
        CHECK_TRUE(found != NULL && found->portNumber == 2222);
        CHECK_TRUE(found != NULL && found->hostKey == "SHA256:abc");
        CHECK_TRUE(found != NULL && found->remoteDirectory == "/srv/www");
        /* Lookup is case-insensitive and finds a bare host name too. */
        CHECK_TRUE(store.Find("SERVERS/WEB.example.com") != NULL);
        CHECK_TRUE(store.Find("web.example.com") != NULL);
        CHECK_TRUE(store.Find("no-such-session") == NULL);

        SessionOptions defaults;
        defaults.portNumber = 22;
        defaults.protocol = "sftp";
        SessionOptions options = SessionStore::ToSessionOptions(*found, defaults);
        CHECK_EQ(options.hostName, "web.example.com");
        CHECK_TRUE(options.portNumber == 2222);
        CHECK_EQ(options.userName, "alice");
        CHECK_EQ(options.initialDirectory, "/srv/www");

        /* The connection round-trips back into a session. */
        StoredSession back = SessionStore::FromSessionOptions("copy", options,
            "/srv/www", "/home/alice/www");
        CHECK_EQ(back.hostName, "web.example.com");
        CHECK_EQ(back.userName, "alice");
        CHECK_TRUE(back.lastUsed != 0);

        /* A second session, then export and import. */
        StoredSession second;
        second.name = "backup";
        second.hostName = "backup.example.com";
        second.protocol = "sftp";
        std::string error;
        CHECK_TRUE(store.AddOrUpdate(second, error));
        CHECK_TRUE(store.Save(error));
        CHECK_TRUE(store.Sessions().size() == 2);

        const std::string exported = temporary + "/exported.ini";
        CHECK_TRUE(store.ExportFile(exported, error));

        SessionStore importStore(temporary + "/imported.ini");
        const int imported = importStore.ImportFile(exported, true, error);
        CHECK_TRUE(imported == 2);
        CHECK_TRUE(importStore.Find("backup") != NULL);
        CHECK_TRUE(importStore.Find("Servers/web.example.com") != NULL);

        /* Removing one leaves the other. */
        CHECK_TRUE(store.Remove("backup", error));
        CHECK_TRUE(store.Find("backup") == NULL);
        CHECK_FALSE(store.Remove("backup", error));

        CHECK_TRUE(store.Rename("Servers/web.example.com", "Servers/web2", error));
        CHECK_TRUE(store.Find("Servers/web2") != NULL);
        CHECK_TRUE(store.Save(error));

        SessionStore reread(path);
        CHECK_TRUE(reread.Find("Servers/web2") != NULL);
        CHECK_TRUE(reread.Find("backup") == NULL);
    }

    /* Names are validated. */
    CHECK_FALSE(SessionStore::IsValidName(""));
    CHECK_FALSE(SessionStore::IsValidName(" leading"));
    CHECK_FALSE(SessionStore::IsValidName("]"));
    CHECK_TRUE(SessionStore::IsValidName("Servers/web1"));

    RemoveTreeQuiet(temporary);
}

/* ------------------------------------------------------------------ *
 * Remote editing                                                      *
 * ------------------------------------------------------------------ */

void TestEditorHelpers()
{
    const char text[] = "hello\nworld";
    CHECK_TRUE(LooksLikeText(text, sizeof(text) - 1));
    const char binary[] = "he\0llo";
    CHECK_TRUE(LooksLikeText(binary, sizeof(binary) - 1) == false);
    CHECK_TRUE(LooksLikeText("", 0));

    std::string error;
    /* A configured editor that does not exist is an error, not a
     * surprise later on. */
    CHECK_TRUE(ResolveEditorCommand("linuxscp-no-such-editor-xyz", error).empty());
    CHECK_FALSE(error.empty());
    (void)text;
    (void)binary;

#if defined(_WIN32)
    const std::string program = FindExecutableOnPath("cmd");
#else
    const std::string program = "/bin/echo";
#endif
    if (!program.empty())
    {
        const std::vector<std::string> words = ResolveEditorCommand(program, error);
        CHECK_TRUE(!words.empty());
        CHECK_TRUE(error.empty());

        /* An editor command with arguments keeps them. */
        const std::vector<std::string> withArgs =
            ResolveEditorCommand(program + " --wait", error);
        CHECK_TRUE(withArgs.size() == 2);
        CHECK_TRUE(withArgs.size() == 2 && withArgs[1] == "--wait");
    }

    /* The environment decides when nothing is configured. */
    const char * oldEditor = getenv("LINUXSCP_EDITOR");
    const bool hadEditor = oldEditor != NULL;
    const std::string savedEditor = hadEditor ? oldEditor : "";
    SetEnvVar("LINUXSCP_EDITOR", "linuxscp-no-such-editor-xyz");
    CHECK_TRUE(ResolveEditorCommand("", error).empty());
    if (!program.empty())
    {
        SetEnvVar("LINUXSCP_EDITOR", program.c_str());
        CHECK_TRUE(!ResolveEditorCommand("", error).empty());
    }
    SetEnvVar("LINUXSCP_EDITOR", hadEditor ? savedEditor.c_str() : NULL);

    /* The temporary copy lives in a private directory. */
    std::string temporary;
    CHECK_TRUE(PrepareEditorTempFile("/etc/nginx/nginx.conf", temporary, error));
    CHECK_FALSE(temporary.empty());
    CHECK_TRUE(temporary.find("nginx.conf") != std::string::npos);
    const std::string directory = LocalDirName(temporary);
    CHECK_TRUE(StatLocal(directory).isDir);
    RemoveLocalFile(temporary, error);
    RemoveLocalDirectory(directory, error);
}

void TestNewScriptCommands()
{
    const std::string temporary = MakeTempDirectory();
    if (temporary.empty())
    {
        return;
    }
    const char * oldConfig = getenv("LINUXSCP_CONFIG_DIR");
    const bool hadConfig = oldConfig != NULL;
    const std::string savedConfig = hadConfig ? oldConfig : "";
    SetEnvVar("LINUXSCP_CONFIG_DIR", temporary.c_str());
    Console::Instance().SetBatchMode(false);

    /* The store is reachable through the script engine, and an empty one
     * is reported instead of failing. */
    Script script;
    CHECK_TRUE(script.ExecuteLine("session"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.Sessions().Empty());

    CHECK_TRUE(script.ExecuteLine(
        "savesession demo -host=demo.example.com -username=alice -port=2222"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.Sessions().Find("demo") != NULL);

    CHECK_TRUE(script.ExecuteLine("rmsession demo"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.Sessions().Find("demo") == NULL);

    /* Unknown switches and missing names fail explicitly. */
    CHECK_TRUE(script.ExecuteLine("savesession demo -nonsense=1"));
    CHECK_TRUE(script.ErrorCount() == 1);
    CHECK_TRUE(script.ExecuteLine("savesession demo"));
    CHECK_TRUE(script.ErrorCount() == 2);
    CHECK_TRUE(script.ExecuteLine("rmsession"));
    CHECK_TRUE(script.ErrorCount() == 3);
    CHECK_TRUE(script.ExecuteLine("rmsession no-such-session"));
    CHECK_TRUE(script.ErrorCount() == 4);

    /* 'edit' and 'keepuptodate' need a connection and say so. */
    CHECK_TRUE(script.ExecuteLine("edit /tmp/file.txt"));
    CHECK_TRUE(script.ErrorCount() == 5);
    CHECK_TRUE(script.ExecuteLine("keepuptodate local /tmp /tmp"));
    CHECK_TRUE(script.ErrorCount() == 6);

    /* The editor is configurable and reported back. */
    CHECK_TRUE(script.ExecuteLine("option editor vim"));
    CHECK_TRUE(script.ErrorCount() == 6);
    CHECK_TRUE(script.ExecuteLine("option editor"));
    CHECK_TRUE(script.ExecuteLine("option editmaxsize 1000"));
    CHECK_TRUE(script.ExecuteLine("option editmaxsize"));
    CHECK_TRUE(script.ErrorCount() == 6);
    CHECK_TRUE(script.ExecuteLine("option editmaxsize abc"));
    CHECK_TRUE(script.ErrorCount() == 7);

    /* Synchronization validates before it connects, and 'both -delete'
     * is accepted now that the recorded state decides what is obsolete. */
    CHECK_TRUE(script.ExecuteLine("synchronize both -delete /tmp /tmp"));
    CHECK_TRUE(script.ErrorCount() == 8);
    CHECK_TRUE(script.ExecuteLine("synchronize local -criteria=whenever"));
    CHECK_TRUE(script.ErrorCount() == 9);
    CHECK_TRUE(script.ExecuteLine("synchronize local -filemask=sub/*"));
    CHECK_TRUE(script.ErrorCount() == 10);
    CHECK_TRUE(script.ExecuteLine("synchronize local -unknown"));
    CHECK_TRUE(script.ErrorCount() == 11);

    SetEnvVar("LINUXSCP_CONFIG_DIR", hadConfig ? savedConfig.c_str() : NULL);
    RemoveTreeQuiet(temporary);
}

} // namespace

int main()
{
    TestSplitCommandLine();
    TestSplitCommands();
    TestRemotePaths();
    TestLocalPaths();
    TestMasks();
    TestMaskCase();
    TestPlatformServices();
    TestFormatting();
    TestDateParsing();
    TestTerminalCommandLine();
    TestParseSiteUrl();
    TestLocalFs();
    TestSessionRegistry();
    TestScriptCommands();
    TestFileMaskParsing();
    TestSyncPlannerModes();
    TestSyncPlannerCriteria();
    TestSyncPlannerMaskAndRecursion();
    TestSyncPlannerConflictsAndState();
    TestSyncPlanOrder();
    TestSyncStateFile();
    TestSessionStore();
    TestEditorHelpers();
    TestNewScriptCommands();

    if (gFailures != 0)
    {
        printf("%d of %d checks failed\n", gFailures, gChecks);
        return 1;
    }
    printf("All %d checks passed\n", gChecks);
    return 0;
}
