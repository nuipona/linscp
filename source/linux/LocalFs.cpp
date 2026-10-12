#include "LocalFs.h"
#include "PathUtil.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace linuxscp {

LocalFileInfo StatLocal(const std::string & path)
{
    LocalFileInfo result;
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        result.error = errno;
        return result;
    }
    result.error = 0;
    result.exists = true;
    result.isDir = S_ISDIR(st.st_mode);
    result.isRegular = S_ISREG(st.st_mode);
    result.size = static_cast<uint64_t>(st.st_size);
    result.mtime = static_cast<unsigned long>(st.st_mtime);
    result.permissions = static_cast<unsigned long>(st.st_mode);
    return result;
}

bool ListLocalDirectory(const std::string & dir, std::vector<std::string> & names,
    std::string & error)
{
    names.clear();
    error.clear();
    DIR * handle = opendir(dir.c_str());
    if (handle == NULL)
    {
        error = LastSystemError();
        return false;
    }
    struct dirent * entry;
    errno = 0;
    while ((entry = readdir(handle)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        names.push_back(entry->d_name);
        errno = 0;
    }
    const int readError = errno;
    if (closedir(handle) != 0 && readError == 0)
    {
        error = LastSystemError();
        names.clear();
        return false;
    }
    if (readError != 0)
    {
        errno = readError;
        error = LastSystemError();
        names.clear();
        return false;
    }
    return true;
}

bool ExpandLocalWildcard(const std::string & path, std::vector<std::string> & names,
    std::string & error)
{
    names.clear();
    error.clear();
    if (!HasWildcard(path))
    {
        LocalFileInfo info = StatLocal(path);
        if (info.exists)
        {
            names.push_back(path);
        }
        return true;
    }

    std::string dir = LocalDirName(path);
    std::string mask = LocalFileName(path);
    std::vector<std::string> entries;
    if (!ListLocalDirectory(dir, entries, error))
    {
        return false;
    }
    for (size_t i = 0; i < entries.size(); i++)
    {
        if (MatchesMask(mask, entries[i]))
        {
            names.push_back(JoinLocalPath(dir, entries[i]));
        }
    }
    return true;
}

bool CreateLocalDirectory(const std::string & path, std::string & error)
{
    error.clear();
    if (mkdir(path.c_str(), 0777) == 0)
    {
        return true;
    }
    if (errno != EEXIST)
    {
        error = LastSystemError();
        return false;
    }

    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
        {
            return true;
        }
        error = "An object with this name already exists and is not a directory.";
        return false;
    }
    error = LastSystemError();
    return false;
}

bool CreateLocalDirectoryRecursive(const std::string & path, std::string & error)
{
    error.clear();
    if (path.empty())
    {
        return false;
    }
    std::string expanded = ExpandTilde(path);
    LocalFileInfo info = StatLocal(expanded);
    if (info.exists)
    {
        if (info.isDir)
        {
            return true;
        }
        error = "Path '" + expanded + "' exists and is not a directory.";
        return false;
    }
    std::string parent = LocalDirName(expanded);
    if (!parent.empty() && parent != expanded && parent != "." && parent != "/")
    {
        LocalFileInfo parentInfo = StatLocal(parent);
        if (!parentInfo.exists)
        {
            if (!CreateLocalDirectoryRecursive(parent, error))
            {
                return false;
            }
        }
        else if (!parentInfo.isDir)
        {
            error = "Parent path '" + parent + "' exists and is not a directory.";
            return false;
        }
    }
    return CreateLocalDirectory(expanded, error);
}

bool RemoveLocalFile(const std::string & path, std::string & error)
{
    error.clear();
    if (unlink(path.c_str()) != 0)
    {
        error = LastSystemError();
        return false;
    }
    return true;
}

std::string GetLocalDirectory(std::string & error)
{
    error.clear();
    char buffer[PATH_MAX];
    if (getcwd(buffer, sizeof(buffer)) == NULL)
    {
        error = LastSystemError();
        return std::string();
    }
    return std::string(buffer);
}

bool SetLocalDirectory(const std::string & path, std::string & error)
{
    error.clear();
    if (chdir(path.c_str()) != 0)
    {
        error = LastSystemError();
        return false;
    }
    return true;
}

std::string LastSystemError()
{
    const char * text = strerror(errno);
    return text != NULL ? std::string(text) : std::string("Unknown error");
}

} // namespace linuxscp
