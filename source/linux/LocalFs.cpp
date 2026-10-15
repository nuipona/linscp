#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace linuxscp {

namespace {

std::filesystem::path ToPath(const std::string & path)
{
    return std::filesystem::path(path);
}

std::string FromPath(const std::filesystem::path & path)
{
    return path.string();
}

bool LocalNameMatches(const std::string & mask, const std::string & name)
{
    /* Local file systems here distinguish case, so masks do too. */
    return MatchesMask(mask, name);
}

} // namespace

LocalFileInfo StatLocal(const std::string & path)
{
    LocalFileInfo result;
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        result.error = errno;
        return result;
    }
    result.isDir = S_ISDIR(st.st_mode);
    result.isRegular = S_ISREG(st.st_mode);
    result.error = 0;
    result.exists = true;
    result.size = static_cast<uint64_t>(st.st_size);
    result.atime = static_cast<unsigned long>(st.st_atime);
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
        if (LocalNameMatches(mask, entries[i]))
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

    LocalFileInfo info = StatLocal(path);
    if (info.exists)
    {
        if (info.isDir)
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
    if (!parent.empty() && parent != expanded && parent != "." &&
        !(parent.size() == 1 && parent[0] == '/'))
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

bool SetLocalFileMode(const std::string & path, int mode, std::string & error)
{
    error.clear();
    if (chmod(path.c_str(), static_cast<mode_t>(mode & 07777)) != 0)
    {
        error = LastSystemError();
        return false;
    }
    return true;
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

bool RemoveLocalDirectory(const std::string & path, std::string & error)
{
    error.clear();
    std::error_code ec;
    if (!std::filesystem::remove(ToPath(path), ec) || ec)
    {
        if (ec)
        {
            error = ec.message();
        }
        else
        {
            errno = ENOENT;
            error = LastSystemError();
        }
        return false;
    }
    return true;
}

std::string GetLocalDirectory(std::string & error)
{
    error.clear();
    std::error_code ec;
    std::filesystem::path current = std::filesystem::current_path(ec);
    if (ec)
    {
        error = ec.message();
        return std::string();
    }
    return FromPath(current);
}

bool SetLocalDirectory(const std::string & path, std::string & error)
{
    error.clear();
    std::error_code ec;
    std::filesystem::current_path(ToPath(path), ec);
    if (ec)
    {
        error = ec.message();
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
