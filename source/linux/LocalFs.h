/*
 * LocalFs: access to the local file system, used by 'lcd'/'lpwd',
 * by local wildcard expansion and by the local end of a transfer.
 */

#ifndef LINUXSCP_LOCALFS_H
#define LINUXSCP_LOCALFS_H

#include <cstdint>
#include <string>
#include <vector>

namespace linuxscp {

struct LocalFileInfo
{
    bool exists;
    bool isDir;
    bool isRegular;
    uint64_t size;
    unsigned long atime;
    unsigned long mtime;
    unsigned long permissions;
    int error;

    LocalFileInfo() : exists(false), isDir(false), isRegular(false), size(0),
        atime(0), mtime(0), permissions(0), error(0) {}
};

/* Returns path attributes; if stat fails, 'error' contains the errno value. */
LocalFileInfo StatLocal(const std::string & path);

/* Names in a directory, without '.' and '..'. */
bool ListLocalDirectory(const std::string & dir, std::vector<std::string> & names,
    std::string & error);

/* Full paths of the local names matching a (possibly wildcard) mask.
 * A mask without wildcards yields the single path if it exists. */
bool ExpandLocalWildcard(const std::string & path, std::vector<std::string> & names,
    std::string & error);

bool CreateLocalDirectory(const std::string & path, std::string & error);
bool CreateLocalDirectoryRecursive(const std::string & path, std::string & error);
/*
 * Sets the permission bits of a local file, as a download's
 * '-permissions' switch asks. On Windows only the read-only bit of the
 * mode is representable; the call still reports failure honestly.
 */
bool SetLocalFileMode(const std::string & path, int mode, std::string & error);

bool RemoveLocalFile(const std::string & path, std::string & error);
/* Removes an empty local directory. */
bool RemoveLocalDirectory(const std::string & path, std::string & error);

std::string GetLocalDirectory(std::string & error);
bool SetLocalDirectory(const std::string & path, std::string & error);

/* Text of the last errno, e.g. 'Permission denied'. */
std::string LastSystemError();

} // namespace linuxscp

#endif /* LINUXSCP_LOCALFS_H */
