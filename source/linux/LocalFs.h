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
    uint64_t size;
    unsigned long mtime;
    unsigned long permissions;

    LocalFileInfo() : exists(false), isDir(false), size(0), mtime(0), permissions(0) {}
};

/* Returns the attributes of a path; 'exists' is false if there are none. */
LocalFileInfo StatLocal(const std::string & path);

/* Names in a directory, without '.' and '..'. */
bool ListLocalDirectory(const std::string & dir, std::vector<std::string> & names,
    std::string & error);

/* Full paths of the local names matching a (possibly wildcard) mask.
 * A mask without wildcards yields the single path if it exists. */
bool ExpandLocalWildcard(const std::string & path, std::vector<std::string> & names,
    std::string & error);

bool CreateLocalDirectory(const std::string & path, std::string & error);
bool RemoveLocalFile(const std::string & path, std::string & error);

std::string GetLocalDirectory(std::string & error);
bool SetLocalDirectory(const std::string & path, std::string & error);

/* Text of the last errno, e.g. 'Permission denied'. */
std::string LastSystemError();

} // namespace linuxscp

#endif /* LINUXSCP_LOCALFS_H */
