#include "PathUtil.h"

#include "Platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace linuxscp {

namespace {

bool IsLocalSeparator(char c)
{
    if (c == '/')
    {
        return true;
    }
    return kIsWindows && c == '\\';
}

#if defined(_WIN32)

/* Splits a local path into its root prefix ("" for a relative path,
 * "C:", "C:\", "\", "\\server\share\") and the remaining components.
 * A trailing separator of the remaining part is ignored. */
void SplitLocalRoot(const std::string & path, std::string & root,
    std::vector<std::string> & parts)
{
    root.clear();
    parts.clear();
    size_t i = 0;
    if (path.size() >= 2 && path[1] == ':' &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
    {
        root = path.substr(0, 2);
        i = 2;
        if (i < path.size() && IsLocalSeparator(path[i]))
        {
            root += "\\";
            i++;
        }
    }
    else if (path.size() >= 2 && IsLocalSeparator(path[0]) && IsLocalSeparator(path[1]))
    {
        /* UNC: \\server\share\ is the root. */
        size_t serverEnd = i = 2;
        while (serverEnd < path.size() && !IsLocalSeparator(path[serverEnd]))
        {
            serverEnd++;
        }
        std::string server = path.substr(2, serverEnd - 2);
        size_t shareEnd = serverEnd;
        if (shareEnd < path.size())
        {
            shareEnd++;
            while (shareEnd < path.size() && !IsLocalSeparator(path[shareEnd]))
            {
                shareEnd++;
            }
        }
        std::string share = serverEnd < path.size() ?
            path.substr(serverEnd + 1, shareEnd - serverEnd - 1) : std::string();
        root = "\\\\" + server;
        if (!share.empty())
        {
            root += "\\" + share;
        }
        root += "\\";
        i = shareEnd;
    }
    else if (!path.empty() && IsLocalSeparator(path[0]))
    {
        root = "\\";
        i = 1;
    }

    while (i < path.size())
    {
        while (i < path.size() && IsLocalSeparator(path[i]))
        {
            i++;
        }
        size_t end = i;
        while (end < path.size() && !IsLocalSeparator(path[end]))
        {
            end++;
        }
        if (end > i)
        {
            parts.push_back(path.substr(i, end - i));
        }
        i = end;
    }
}

bool IsDriveRooted(const std::string & root)
{
    return root.size() == 3 && root[1] == ':' && root[2] == '\\';
}

bool RootIsAbsolute(const std::string & root)
{
    if (root.empty())
    {
        return false;
    }
    if (root[0] == '\\')
    {
        return true; /* rooted or UNC */
    }
    return IsDriveRooted(root);
}

std::string JoinLocalParts(const std::string & root,
    const std::vector<std::string> & parts)
{
    std::string result = root;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (!result.empty() && result[result.size() - 1] != '\\')
        {
            result += "\\";
        }
        result += parts[i];
    }
    if (result.empty())
    {
        result = ".";
    }
    return result;
}

#endif /* _WIN32 */

bool MatchesMaskImpl(const std::string & mask, const std::string & name,
    bool insensitive)
{
    /* Greedy wildcard matching with backtracking to the most recent '*'.
     * This avoids recursive stack growth on long names and masks, while
     * using only constant auxiliary memory. */
    auto equal = [insensitive](char a, char b) {
        if (insensitive)
        {
            if (a >= 'A' && a <= 'Z')
            {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = static_cast<char>(b - 'A' + 'a');
            }
        }
        return a == b;
    };

    size_t maskIndex = 0;
    size_t nameIndex = 0;
    size_t lastStar = std::string::npos;
    size_t retryNameIndex = 0;

    while (nameIndex < name.size())
    {
        if (maskIndex < mask.size() &&
            (mask[maskIndex] == '?' || equal(mask[maskIndex], name[nameIndex])))
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

} // namespace

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
                if (!kIsWindows && quote == '"' && line[i] == '\\' &&
                    i + 1 < line.size())
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
        else if (!kIsWindows && c == '\\' && i + 1 < line.size())
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
            if (!kIsWindows && c == '\\' && quote == '"' && i + 1 < text.size())
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
        else if (!kIsWindows && c == '\\' && i + 1 < text.size())
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
    std::string home = HomeDirectory();
    if (home.empty())
    {
        return path;
    }
    if (path.size() == 1)
    {
        return home;
    }
    if (path[1] != '/' && !(kIsWindows && path[1] == '\\'))
    {
        return path; // '~user' is not supported
    }
    return JoinLocalPath(home, path.substr(2));
}

bool IsLocalAbsolutePath(const std::string & path)
{
#if defined(_WIN32)
    std::string root;
    std::vector<std::string> parts;
    SplitLocalRoot(path, root, parts);
    return RootIsAbsolute(root);
#else
    return !path.empty() && path[0] == '/';
#endif
}

std::string LocalDirName(const std::string & path)
{
#if defined(_WIN32)
    std::string stripped = path;
    while (stripped.size() > 1 && IsLocalSeparator(stripped[stripped.size() - 1]))
    {
        stripped.erase(stripped.size() - 1);
    }
    std::string::size_type pos = stripped.find_last_of("\\/");
    if (pos == std::string::npos)
    {
        /* A bare drive ('C:file') lives in the drive's current dir. */
        if (stripped.size() >= 2 && stripped[1] == ':')
        {
            return stripped.substr(0, 2);
        }
        return std::string(".");
    }
    if (pos == 0)
    {
        return std::string("\\");
    }
    if (pos == 2 && stripped[1] == ':')
    {
        return stripped.substr(0, 3); /* keep 'C:\' */
    }
    /* A UNC share root ('\\server\share') is its own directory. */
    std::string root;
    std::vector<std::string> parts;
    SplitLocalRoot(stripped, root, parts);
    if (parts.empty() && root.size() > 1 && root[0] == '\\')
    {
        return stripped;
    }
    return stripped.substr(0, pos);
#else
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
#endif
}

std::string LocalFileName(const std::string & path)
{
#if defined(_WIN32)
    std::string stripped = path;
    while (stripped.size() > 1 && IsLocalSeparator(stripped[stripped.size() - 1]))
    {
        stripped.erase(stripped.size() - 1);
    }
    std::string::size_type pos = stripped.find_last_of("\\/");
    if (pos == std::string::npos)
    {
        if (stripped.size() > 2 && stripped[1] == ':')
        {
            return stripped.substr(2);
        }
        return stripped;
    }
    return stripped.substr(pos + 1);
#else
    std::string::size_type pos = path.find_last_of('/');
    if (pos == std::string::npos)
    {
        return path;
    }
    return path.substr(pos + 1);
#endif
}

std::string JoinLocalPath(const std::string & dir, const std::string & name)
{
    if (name.empty())
    {
        return dir;
    }
#if defined(_WIN32)
    if (IsLocalAbsolutePath(name))
    {
        /* A rooted name ('\dir') still belongs to dir's drive/share. */
        if ((name[0] == '\\' && (name.size() < 2 || name[1] != '\\')))
        {
            std::string root;
            std::vector<std::string> parts;
            SplitLocalRoot(dir, root, parts);
            if (!root.empty() && root[root.size() - 1] != '\\')
            {
                root += "\\";
            }
            return root + name.substr(1);
        }
        return name;
    }
    /* A drive-relative name on another drive cannot be joined. */
    if (name.size() >= 2 && name[1] == ':')
    {
        return name;
    }
#endif
    if (!name.empty() && !kIsWindows && name[0] == '/')
    {
        return name;
    }
    if (dir.empty())
    {
        return name;
    }
    if (IsLocalSeparator(dir[dir.size() - 1]))
    {
        return dir + name;
    }
    return dir + kLocalPathSeparator + name;
}

std::string ResolveLocalPath(const std::string & cwd, const std::string & path)
{
    std::string expanded = ExpandTilde(path);
#if defined(_WIN32)
    std::string root;
    std::vector<std::string> parts;
    SplitLocalRoot(expanded, root, parts);
    if (!RootIsAbsolute(root))
    {
        if (root.empty())
        {
            SplitLocalRoot(cwd, root, parts);
        }
        else
        {
            /* 'C:file' is relative to the current directory of that
             * drive, which we do not track: resolve it against cwd
             * when the drive matches, otherwise keep the drive root. */
            std::string cwdRoot;
            std::vector<std::string> cwdParts;
            SplitLocalRoot(cwd, cwdRoot, cwdParts);
            if (cwdRoot.size() >= 2 && root.size() >= 2 &&
                cwdRoot[0] == root[0] && cwdRoot[1] == ':')
            {
                root = cwdRoot;
                parts.insert(parts.begin(), cwdParts.begin(), cwdParts.end());
            }
            else
            {
                root += "\\";
            }
        }
    }
    std::vector<std::string> resolved;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (parts[i] == "..")
        {
            if (!resolved.empty())
            {
                resolved.pop_back();
            }
        }
        else if (parts[i] != "." && !parts[i].empty())
        {
            resolved.push_back(parts[i]);
        }
    }
    return JoinLocalParts(root, resolved);
#else
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
    // tree of a LinuxSCP transfer is always POSIX on this platform.
    return ResolveRemotePath(cwd, joined);
#endif
}

bool HasWildcard(const std::string & path)
{
    return path.find('*') != std::string::npos ||
        path.find('?') != std::string::npos;
}

bool MatchesMask(const std::string & mask, const std::string & name)
{
    return MatchesMaskImpl(mask, name, false);
}

bool MatchesMaskInsensitive(const std::string & mask, const std::string & name)
{
    return MatchesMaskImpl(mask, name, true);
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
    /* No "%e" here: MSVC strftime does not support it. The month name
     * and padding are produced manually so both platforms agree. */
    static const char * months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    time_t t = static_cast<time_t>(time);
    struct tm tmResult;
    if (!LocalTime(t, tmResult))
    {
        return "unknown";
    }
    char buffer[32];
    time_t now = ::time(NULL);
    // Within six months show the time, otherwise the year, like 'ls -l'.
    if (t > now - 180 * 24 * 60 * 60 && t < now + 180 * 24 * 60 * 60)
    {
        snprintf(buffer, sizeof(buffer), "%s %2d %02d:%02d",
            months[tmResult.tm_mon % 12], tmResult.tm_mday,
            tmResult.tm_hour, tmResult.tm_min);
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "%s %2d  %d",
            months[tmResult.tm_mon % 12], tmResult.tm_mday,
            tmResult.tm_year + 1900);
    }
    return std::string(buffer);
}

} // namespace linuxscp
