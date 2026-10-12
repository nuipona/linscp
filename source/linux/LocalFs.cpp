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
        return result;
    }
    result.exists = true;
    result.isDir = S_ISDIR(st.st_mode);
    result.size = static_cast<uint64_t>(st.st_size);
    result.mtime = static_cast<unsigned long>(st.st_mtime);
    result.permissions = static_cast<unsigned long>(st.st_mode);
    return result;
}

bool ListLocalDirectory(const std::string & dir, std::vector<std::string> & names,
    std::string & error)
{
    names.clear();
    DIR * handle = opendir(dir.c_str());
    if (handle == NULL)
    {
        error = LastSystemError();
        return false;
    }
    struct dirent * entry;
    while ((entry = readdir(handle)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        names.push_back(entry->d_name);
    }
    closedir(handle);
    return true;
}

bool ExpandLocalWildcard(const std::string & path, std::vector<std::string> & names,
    std::string & error)
{
    names.clear();
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
    if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST)
    {
        error = LastSystemError();
        return false;
    }
    return true;
}

bool RemoveLocalFile(const std::string & path, std::string & error)
{
    if (unlink(path.c_str()) != 0)
    {
        error = LastSystemError();
        return false;
    }
    return true;
}

std::string GetLocalDirectory(std::string & error)
{
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
