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

#include "PathUtil.h"
#include "LocalFs.h"
#include "Platform.h"
#include "Session.h"
#include "Script.h"

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

    FILE * file = fopen((dir + "/one.txt").c_str(), "w");
    CHECK_TRUE(file != NULL);
    if (file != NULL)
    {
        fputs("hello\n", file);
        fclose(file);
    }
    file = fopen((dir + "/two.log").c_str(), "w");
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
    CHECK_TRUE(script.ExecuteLine("option reconnect 10"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_TRUE(script.ExecuteLine("option transfer binary"));
    CHECK_TRUE(script.ErrorCount() == 0);
    CHECK_FALSE(script.ExecuteLine("exit"));
    CHECK_TRUE(script.ExitRequested());
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
    TestParseSiteUrl();
    TestLocalFs();
    TestSessionRegistry();
    TestScriptCommands();

    if (gFailures != 0)
    {
        printf("%d of %d checks failed\n", gFailures, gChecks);
        return 1;
    }
    printf("All %d checks passed\n", gChecks);
    return 0;
}
