/*
 * SessionStore: reading and writing the persistent site list.
 *
 * The on-disk format is intentionally plain INI with '=' separated
 * values, one section per session, so that the graphical client (which
 * reads the same file from Python) and a user with a text editor see
 * the same thing. Values escape '\\', newlines, carriage returns and
 * tabs; everything else is stored verbatim, UTF-8 encoded.
 */

#include "SessionStore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <algorithm>

#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"

namespace linuxscp {

namespace {

std::string Trim(const std::string & text)
{
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return std::string();
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

std::string EscapeValue(const std::string & text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); i++)
    {
        const char c = text[i];
        if (c == '\\')
        {
            result += "\\\\";
        }
        else if (c == '\n')
        {
            result += "\\n";
        }
        else if (c == '\r')
        {
            result += "\\r";
        }
        else if (c == '\t')
        {
            result += "\\t";
        }
        else
        {
            result += c;
        }
    }
    return result;
}

std::string UnescapeValue(const std::string & text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] != '\\' || i + 1 == text.size())
        {
            result += text[i];
            continue;
        }
        const char c = text[++i];
        result += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
    }
    return result;
}

std::string Lower(const std::string & text)
{
    std::string result = text;
    for (size_t i = 0; i < result.size(); i++)
    {
        if (result[i] >= 'A' && result[i] <= 'Z')
        {
            result[i] = static_cast<char>(result[i] - 'A' + 'a');
        }
    }
    return result;
}

std::string ValueOf(const StoredSession & session, const std::string & key,
    bool * isNumber)
{
    *isNumber = false;
    if (key == "protocol") { return session.protocol; }
    if (key == "host") { return session.hostName; }
    if (key == "username") { return session.userName; }
    if (key == "private_key") { return session.privateKeyFile; }
    if (key == "hostkey") { return session.hostKey; }
    if (key == "remote_directory") { return session.remoteDirectory; }
    if (key == "local_directory") { return session.localDirectory; }
    if (key == "password") { return session.password; }
    if (key == "port")
    {
        *isNumber = true;
        return session.portNumber > 0 ? std::to_string(session.portNumber) : std::string();
    }
    if (key == "last_used")
    {
        *isNumber = true;
        return session.lastUsed != 0 ? std::to_string(session.lastUsed) : std::string();
    }
    return std::string();
}

} // namespace

SessionStore::SessionStore() : mPath(DefaultPath())
{
    std::string error;
    Load(error);
}

SessionStore::SessionStore(const std::string & path) : mPath(path)
{
    std::string error;
    Load(error);
}

std::string SessionStore::DefaultPath()
{
    const std::string config = ConfigDirectory();
    if (config.empty())
    {
        return std::string();
    }
    return config + "/sessions.ini";
}

bool SessionStore::IsValidName(const std::string & name)
{
    if (name.empty() || name.size() > 512)
    {
        return false;
    }
    if (name.find_first_not_of(" \t") == std::string::npos)
    {
        return false;
    }
    if (name[0] == ' ' || name[0] == '\t' ||
        name[name.size() - 1] == ' ' || name[name.size() - 1] == '\t')
    {
        return false;
    }
    for (size_t i = 0; i < name.size(); i++)
    {
        if (static_cast<unsigned char>(name[i]) < 0x20 || name[i] == ']')
        {
            return false;
        }
    }
    return true;
}

const StoredSession * SessionStore::Find(const std::string & name) const
{
    if (name.empty())
    {
        return NULL;
    }
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        if (mSessions[i].name == name)
        {
            return &mSessions[i];
        }
    }
    const std::string wanted = Lower(Trim(name));
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        if (Lower(mSessions[i].name) == wanted)
        {
            return &mSessions[i];
        }
    }
    /* A bare host name is a reasonable way to name a session, so let
     * 'open web.example.com' find the session of that host. */
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        if (Lower(mSessions[i].hostName) == wanted)
        {
            return &mSessions[i];
        }
    }
    return NULL;
}

bool SessionStore::AddOrUpdate(const StoredSession & session, std::string & error)
{
    if (!IsValidName(session.name))
    {
        error = "The session name '" + session.name + "' is not valid.";
        return false;
    }
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        if (Lower(mSessions[i].name) == Lower(session.name))
        {
            mSessions[i] = session;
            return true;
        }
    }
    mSessions.push_back(session);
    return true;
}

bool SessionStore::Remove(const std::string & name, std::string & error)
{
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        if (Lower(mSessions[i].name) == Lower(name))
        {
            mSessions.erase(mSessions.begin() + static_cast<long>(i));
            return true;
        }
    }
    error = "There is no stored session named '" + name + "'.";
    return false;
}

bool SessionStore::Rename(const std::string & from, const std::string & to,
    std::string & error)
{
    if (!IsValidName(to))
    {
        error = "The session name '" + to + "' is not valid.";
        return false;
    }
    const StoredSession * existing = Find(from);
    if (existing == NULL)
    {
        error = "There is no stored session named '" + from + "'.";
        return false;
    }
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        if (&mSessions[i] == existing)
        {
            mSessions[i].name = to;
            return true;
        }
    }
    error = "There is no stored session named '" + from + "'.";
    return false;
}

bool SessionStore::Load(std::string & error)
{
    error.clear();
    mSessions.clear();
    if (mPath.empty())
    {
        return true;
    }
    FILE * file = fopen(mPath.c_str(), "rb");
    if (file == NULL)
    {
        return true; /* no stored sessions yet */
    }

    StoredSession current;
    bool inSection = false;
    char buffer[8192];
    while (fgets(buffer, sizeof(buffer), file) != NULL)
    {
        std::string line(buffer);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
        {
            line.erase(line.size() - 1);
        }
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
        {
            continue;
        }
        if (trimmed[0] == '[' && trimmed[trimmed.size() - 1] == ']')
        {
            if (inSection)
            {
                mSessions.push_back(current);
            }
            current = StoredSession();
            current.name = UnescapeValue(trimmed.substr(1, trimmed.size() - 2));
            inSection = true;
            continue;
        }
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
        {
            continue; /* not a line this format understands: ignore it */
        }
        if (!inSection)
        {
            continue; /* a value outside any section */
        }
        const std::string key = Lower(Trim(trimmed.substr(0, equals)));
        const std::string value = UnescapeValue(Trim(trimmed.substr(equals + 1)));
        if (key == "port")
        {
            current.portNumber = atoi(value.c_str());
        }
        else if (key == "last_used")
        {
            current.lastUsed = strtoull(value.c_str(), NULL, 10);
        }
        else if (key == "protocol") { current.protocol = value; }
        else if (key == "host" || key == "hostname") { current.hostName = value; }
        else if (key == "username" || key == "user") { current.userName = value; }
        else if (key == "private_key" || key == "privatekey" || key == "keyfile")
        {
            current.privateKeyFile = value;
        }
        else if (key == "hostkey" || key == "host_key") { current.hostKey = value; }
        else if (key == "remote_directory" || key == "remotedir")
        {
            current.remoteDirectory = value;
        }
        else if (key == "local_directory" || key == "localdir")
        {
            current.localDirectory = value;
        }
        else if (key == "password") { current.password = value; }
    }
    if (inSection)
    {
        mSessions.push_back(current);
    }
    fclose(file);

    std::sort(mSessions.begin(), mSessions.end(),
        [](const StoredSession & left, const StoredSession & right)
        {
            return Lower(left.name) < Lower(right.name);
        });
    return true;
}

bool SessionStore::Save(std::string & error) const
{
    error.clear();
    if (mPath.empty())
    {
        error = "There is no configuration directory, so the session cannot "
            "be stored (set HOME, or LINUXSCP_CONFIG_DIR to choose one).";
        return false;
    }
    const size_t slash = mPath.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0)
    {
        const std::string directory = mPath.substr(0, slash);
        std::string createError;
        if (!CreatePrivateDirectory(directory, createError))
        {
            error = createError;
            return false;
        }
    }
    FILE * file = fopen(mPath.c_str(), "wb");
    if (file == NULL)
    {
        error = std::string("Cannot write the session file '") + mPath + "'.";
        return false;
    }
    fprintf(file, "# LinuxSCP stored sessions. Names may contain '/' to group\n");
    fprintf(file, "# sessions into folders. Passwords are only stored when\n");
    fprintf(file, "# they are saved explicitly, and then in this file only.\n");
    std::vector<StoredSession> sorted = mSessions;
    std::sort(sorted.begin(), sorted.end(),
        [](const StoredSession & left, const StoredSession & right)
        {
            return Lower(left.name) < Lower(right.name);
        });
    for (size_t i = 0; i < sorted.size(); i++)
    {
        fprintf(file, "\n[%s]\n", EscapeValue(sorted[i].name).c_str());
        static const char * const keys[] = {
            "protocol", "host", "port", "username", "private_key", "hostkey",
            "remote_directory", "local_directory", "password", "last_used"
        };
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
        {
            bool isNumber = false;
            const std::string value = ValueOf(sorted[i], keys[k], &isNumber);
            if (value.empty())
            {
                continue;
            }
            fprintf(file, "%s=%s\n", keys[k], EscapeValue(value).c_str());
            (void)isNumber;
        }
    }
    fclose(file);
    if (!sorted.empty())
    {
        const bool hasPassword = [&sorted]()
        {
            for (size_t i = 0; i < sorted.size(); i++)
            {
                if (!sorted[i].password.empty())
                {
                    return true;
                }
            }
            return false;
        }();
        if (hasPassword)
        {
            RestrictFileToUser(mPath);
        }
    }
    return true;
}

int SessionStore::ImportFile(const std::string & path, bool merge, std::string & error)
{
    SessionStore imported(path);
    int count = 0;
    for (size_t i = 0; i < imported.Sessions().size(); i++)
    {
        const StoredSession & session = imported.Sessions()[i];
        bool replaced = false;
        for (size_t k = 0; k < mSessions.size(); k++)
        {
            if (Lower(mSessions[k].name) == Lower(session.name))
            {
                if (!merge)
                {
                    mSessions[k] = session;
                }
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            mSessions.push_back(session);
        }
        count++;
    }
    (void)error;
    return count;
}

bool SessionStore::ExportFile(const std::string & path, std::string & error)
{
    SessionStore copy(path);
    for (size_t i = 0; i < mSessions.size(); i++)
    {
        std::string addError;
        if (!copy.AddOrUpdate(mSessions[i], addError))
        {
            error = addError;
            return false;
        }
    }
    return copy.Save(error);
}

SessionOptions SessionStore::ToSessionOptions(const StoredSession & session,
    const SessionOptions & defaults)
{
    SessionOptions options = defaults;
    if (!session.protocol.empty())
    {
        options.protocol = session.protocol;
    }
    if (!session.hostName.empty())
    {
        options.hostName = session.hostName;
    }
    if (session.portNumber > 0)
    {
        options.portNumber = session.portNumber;
    }
    if (!session.userName.empty())
    {
        options.userName = session.userName;
    }
    if (!session.privateKeyFile.empty())
    {
        options.privateKeyFile = session.privateKeyFile;
    }
    if (!session.hostKey.empty())
    {
        options.hostKey = session.hostKey;
    }
    options.initialDirectory = session.remoteDirectory;
    if (!session.password.empty())
    {
        options.password = session.password;
    }
    return options;
}

StoredSession SessionStore::FromSessionOptions(const std::string & name,
    const SessionOptions & options, const std::string & remoteDirectory,
    const std::string & localDirectory)
{
    StoredSession session;
    session.name = name;
    session.protocol = options.protocol;
    session.hostName = options.hostName;
    session.portNumber = options.portNumber;
    session.userName = options.userName;
    session.privateKeyFile = options.privateKeyFile;
    session.hostKey = options.hostKey;
    session.remoteDirectory = remoteDirectory;
    session.localDirectory = localDirectory;
    session.lastUsed = static_cast<unsigned long long>(time(NULL));
    return session;
}

} // namespace linuxscp
