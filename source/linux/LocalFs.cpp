#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace linuxscp {

namespace {

#if defined(_WIN32)
/* Internal strings are UTF-8; std::filesystem wants UTF-8 paths on
 * Windows through u8path. */
std::filesystem::path ToPath(const std::string & path)
{
    return std::filesystem::u8path(path);
}

std::string FromPath(const std::filesystem::path & path)
{
    return path.u8string();
}
#else
std::filesystem::path ToPath(const std::string & path)
{
    return std::filesystem::path(path);
}

std::string FromPath(const std::filesystem::path & path)
{
    return path.string();
}
#endif

bool LocalNameMatches(const std::string & mask, const std::string & name)
{
    /* Windows volumes do not distinguish case; POSIX ones do. */
    return kIsWindows ? MatchesMaskInsensitive(mask, name) :
        MatchesMask(mask, name);
}

} // namespace

LocalFileInfo StatLocal(const std::string & path)
{
    LocalFileInfo result;
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0)
    {
        result.error = errno;
        return result;
    }
    result.isDir = (st.st_mode & _S_IFMT) == _S_IFDIR;
    result.isRegular = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        result.error = errno;
        return result;
    }
    result.isDir = S_ISDIR(st.st_mode);
    result.isRegular = S_ISREG(st.st_mode);
#endif
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
#if defined(_WIN32)
    std::error_code ec;
    std::filesystem::directory_iterator it(ToPath(dir), ec);
    if (ec)
    {
        /* std::filesystem reports Win32 error codes here; translate the
         * common one so callers see the same text as on POSIX. */
        std::error_code probe;
        errno = !std::filesystem::exists(ToPath(dir), probe) ? ENOENT : EIO;
        error = LastSystemError();
        return false;
    }
    for (const auto & entry : it)
    {
        names.push_back(FromPath(entry.path().filename()));
    }
    return true;
#else
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
#endif
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
#if defined(_WIN32)
    if (_mkdir(path.c_str()) == 0)
    {
        return true;
    }
#else
    if (mkdir(path.c_str(), 0777) == 0)
    {
        return true;
    }
#endif
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
        !(parent.size() == 1 && parent[0] == kLocalPathSeparator))
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
#if defined(_WIN32)
    /* _unlink fails on read-only files where POSIX unlink succeeds;
     * clear the read-only bit first, as Windows users expect. */
    _chmod(path.c_str(), _S_IREAD | _S_IWRITE);
    if (_unlink(path.c_str()) != 0)
#else
    if (unlink(path.c_str()) != 0)
#endif
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
