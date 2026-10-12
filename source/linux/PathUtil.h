/*
 * PathUtil: path handling shared by the POSIX front end.
 *
 * Remote (server side) paths are always POSIX: '/' separated, always
 * absolute once resolved against the current remote directory. Local
 * paths use the same convention on Linux, but the two are kept in
 * separate helpers so that the Windows front end (which needs '\' and
 * drive letters) can reuse the remote half unchanged.
 */

#ifndef LINUXSCP_PATHUTIL_H
#define LINUXSCP_PATHUTIL_H

#include <cstdint>
#include <string>
#include <vector>

namespace linuxscp {

/* Splits a script/command line into words. Handles single quotes, double
 * quotes and backslash escapes; the quotes themselves are not kept. */
std::vector<std::string> SplitCommandLine(const std::string & line);
/* Splits a command line holding several commands separated by ';' into the
 * individual commands. ';' inside quotes is kept. */
std::vector<std::string> SplitCommands(const std::string & text);

/* Remote paths. */
bool IsRemoteAbsolutePath(const std::string & path);
std::string RemoteDirName(const std::string & path);
std::string RemoteFileName(const std::string & path);
/* Joins a directory and a name, adding exactly one '/'. */
std::string JoinRemotePath(const std::string & dir, const std::string & name);
/* Resolves '.'/'..' components and relative paths against cwd. */
std::string ResolveRemotePath(const std::string & cwd, const std::string & path);

/* Local paths. */
std::string ExpandTilde(const std::string & path);
std::string LocalDirName(const std::string & path);
std::string LocalFileName(const std::string & path);
std::string JoinLocalPath(const std::string & dir, const std::string & name);
std::string ResolveLocalPath(const std::string & cwd, const std::string & path);
/* True if the path contains the wildcards we support in file masks. */
bool HasWildcard(const std::string & path);
/* Case sensitive match of a file mask ('*' and '?') against a file name.
 * A mask with no wildcard matches only exactly that name. */
bool MatchesMask(const std::string & mask, const std::string & name);

/* Formatting helpers used by the console output. */
std::string FormatSize(uint64_t size);
std::string FormatPermissions(unsigned long permissions);
std::string FormatFileTime(unsigned long time);

} // namespace linuxscp

#endif /* LINUXSCP_PATHUTIL_H */
