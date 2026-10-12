#include "PathUtil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace linuxscp {

std::vector<std::string> SplitCommandLine(const std::string & line)
{
    std::vector<std::string> result;
    std::string word;
    bool haveWord = false;
    size_t i = 0;
    while (i < line.size())
    {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (haveWord)
            {
                result.push_back(word);
                word.clear();
                haveWord = false;
            }
            i++;
        }
        else if (c == '\'' || c == '"')
        {
            char quote = c;
            haveWord = true;
            i++;
            while (i < line.size() && line[i] != quote)
            {
                if (quote == '"' && line[i] == '\\' && i + 1 < line.size())
                {
                    i++;
                }
                word += line[i];
                i++;
            }
            if (i < line.size())
            {
                i++; // skip the closing quote
            }
        }
        else if (c == '\\' && i + 1 < line.size())
        {
            haveWord = true;
            i++;
            word += line[i];
            i++;
        }
        else
        {
            haveWord = true;
            word += c;
            i++;
        }
    }
    if (haveWord)
    {
        result.push_back(word);
    }
    return result;
}

std::vector<std::string> SplitCommands(const std::string & text)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        if (quote != 0)
        {
            if (c == '\\' && quote == '"' && i + 1 < text.size())
            {
                current += c;
                i++;
                current += text[i];
                continue;
            }
            if (c == quote)
            {
                quote = 0;
            }
            current += c;
        }
        else if (c == '\'' || c == '"')
        {
            quote = c;
            current += c;
        }
        else if (c == '\\' && i + 1 < text.size())
        {
            current += c;
            i++;
            current += text[i];
        }
        else if (c == ';')
        {
            result.push_back(current);
            current.clear();
        }
        else
        {
            current += c;
        }
    }
    if (!current.empty())
    {
        result.push_back(current);
    }
    return result;
}

bool IsRemoteAbsolutePath(const std::string & path)
{
    return !path.empty() && path[0] == '/';
}

std::string RemoteDirName(const std::string & path)
{
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos)
    {
        return std::string();
    }
    if (pos == 0)
    {
        return std::string("/");
    }
    return path.substr(0, pos);
}

std::string RemoteFileName(const std::string & path)
{
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos)
    {
        return path;
    }
    return path.substr(pos + 1);
}

std::string JoinRemotePath(const std::string & dir, const std::string & name)
{
    if (name.empty())
    {
        return dir;
    }
    if (IsRemoteAbsolutePath(name))
    {
        return name;
    }
    if (dir.empty())
    {
        return name;
    }
    if (dir[dir.size() - 1] == '/')
    {
        return dir + name;
    }
    return dir + "/" + name;
}

/* Resolves a possibly relative path against cwd, collapsing '.' and '..'.
 * A trailing slash of the input is not preserved unless the result is
 * the root directory. */
std::string ResolveRemotePath(const std::string & cwd, const std::string & path)
{
    std::string full;
    std::vector<std::string> parts;
    if (IsRemoteAbsolutePath(path))
    {
        full = path;
    }
    else
    {
        full = JoinRemotePath(cwd.empty() ? std::string("/") : cwd, path);
    }

    size_t i = 0;
    while (i < full.size())
    {
        if (full[i] == '/')
        {
            i++;
            continue;
        }
        size_t end = full.find('/', i);
        if (end == std::string::npos)
        {
            end = full.size();
        }
        std::string part = full.substr(i, end - i);
        if (part == "..")
        {
            if (!parts.empty())
            {
                parts.pop_back();
            }
        }
        else if (part != "." && !part.empty())
        {
            parts.push_back(part);
        }
        i = end;
    }

    std::string result;
    for (size_t n = 0; n < parts.size(); n++)
    {
        result += "/" + parts[n];
    }
    if (result.empty())
    {
        result = "/";
    }
    return result;
}

std::string ExpandTilde(const std::string & path)
{
    if (path.empty() || path[0] != '~')
    {
        return path;
    }
    const char * home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
    {
        return path;
    }
    if (path.size() == 1)
    {
        return std::string(home);
    }
    if (path[1] != '/')
    {
        return path; // '~user' is not supported
    }
    return JoinLocalPath(home, path.substr(2));
}

std::string LocalDirName(const std::string & path)
{
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos)
    {
        return std::string(".");
    }
    if (pos == 0)
    {
        return std::string("/");
    }
    return path.substr(0, pos);
}

std::string LocalFileName(const std::string & path)
{
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos)
    {
        return path;
    }
    return path.substr(pos + 1);
}

std::string JoinLocalPath(const std::string & dir, const std::string & name)
{
    if (name.empty())
    {
        return dir;
    }
    if (!name.empty() && name[0] == '/')
    {
        return name;
    }
    if (dir.empty())
    {
        return name;
    }
    if (dir[dir.size() - 1] == '/')
    {
        return dir + name;
    }
    return dir + "/" + name;
}

std::string ResolveLocalPath(const std::string & cwd, const std::string & path)
{
    std::string expanded = ExpandTilde(path);
    if (!expanded.empty() && expanded[0] == '/')
    {
        return expanded;
    }
    if (expanded == ".")
    {
        return cwd;
    }
    std::string joined = JoinLocalPath(cwd, expanded);
    // Collapse './' and '../' the same way as for remote paths; the local
    // tree of a LinuxSCP transfer is always POSIX.
    return ResolveRemotePath(cwd, joined);
}

bool HasWildcard(const std::string & path)
{
    return path.find('*') != std::string::npos ||
        path.find('?') != std::string::npos;
}

bool MatchesMask(const std::string & mask, const std::string & name)
{
    /* Greedy wildcard matching with backtracking to the most recent '*'.
     * This avoids recursive stack growth on long names and masks, while
     * using only constant auxiliary memory. */
    size_t maskIndex = 0;
    size_t nameIndex = 0;
    size_t lastStar = std::string::npos;
    size_t retryNameIndex = 0;

    while (nameIndex < name.size())
    {
        if (maskIndex < mask.size() &&
            (mask[maskIndex] == '?' || mask[maskIndex] == name[nameIndex]))
        {
            maskIndex++;
            nameIndex++;
        }
        else if (maskIndex < mask.size() && mask[maskIndex] == '*')
        {
            lastStar = maskIndex++;
            retryNameIndex = nameIndex;
        }
        else if (lastStar != std::string::npos)
        {
            maskIndex = lastStar + 1;
            nameIndex = ++retryNameIndex;
        }
        else
        {
            return false;
        }
    }

    while (maskIndex < mask.size() && mask[maskIndex] == '*')
    {
        maskIndex++;
    }
    return maskIndex == mask.size();
}

std::string FormatSize(uint64_t size)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llu",
        static_cast<unsigned long long>(size));
    return std::string(buffer);
}

std::string FormatPermissions(unsigned long permissions)
{
    static const char * bits = "rwxrwxrwx";
    std::string result;
    result += (permissions & 0040000) ? 'd' : '-';
    for (int i = 0; i < 9; i++)
    {
        result += (permissions & (0400 >> i)) ? bits[i] : '-';
    }
    if (permissions & 0004000)
    {
        result[3] = (permissions & 0000100) ? 's' : 'S';
    }
    if (permissions & 0002000)
    {
        result[6] = (permissions & 0000010) ? 's' : 'S';
    }
    if (permissions & 0001000)
    {
        result[9] = (permissions & 0000001) ? 't' : 'T';
    }
    return result;
}

std::string FormatFileTime(unsigned long time)
{
    time_t t = static_cast<time_t>(time);
    struct tm tmResult;
    if (localtime_r(&t, &tmResult) == NULL)
    {
        return "unknown";
    }
    char buffer[32];
    time_t now = ::time(NULL);
    // Within six months show the time, otherwise the year, like 'ls -l'.
    if (t > now - 180 * 24 * 60 * 60 && t < now + 180 * 24 * 60 * 60)
    {
        strftime(buffer, sizeof(buffer), "%b %e %H:%M", &tmResult);
    }
    else
    {
        strftime(buffer, sizeof(buffer), "%b %e  %Y", &tmResult);
    }
    return std::string(buffer);
}

} // namespace linuxscp
