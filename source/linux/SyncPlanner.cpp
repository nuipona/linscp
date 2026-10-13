/*
 * SyncPlanner: the comparison and planning engine of 'synchronize' and
 * 'keepuptodate'.
 *
 * The planner reads both trees through SyncFileSystem and returns a
 * plan; see SyncPlanner.h for the rules. Nothing here transfers,
 * creates or removes anything, which is what allows 'synchronize
 * -preview' to be completely safe and 'synchronize both -delete' to be
 * driven by the recorded state instead of by a guess.
 */

#include "SyncPlanner.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>

#include <algorithm>
#include <set>

#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"

namespace linuxscp {

namespace {

unsigned long long NowSeconds()
{
    return static_cast<unsigned long long>(time(NULL));
}

} // namespace

const char * SyncModeName(SyncMode mode)
{
    switch (mode)
    {
        case SyncMode::Local:  return "local";
        case SyncMode::Remote: return "remote";
        case SyncMode::Both:   return "both";
    }
    return "local";
}

const char * SyncCriteriaName(SyncCriteria criteria)
{
    switch (criteria)
    {
        case SyncCriteria::Time:   return "time";
        case SyncCriteria::Size:   return "size";
        case SyncCriteria::Either: return "either";
        case SyncCriteria::None:   return "none";
    }
    return "time";
}

const char * SyncActionName(SyncAction action)
{
    switch (action)
    {
        case SyncAction::None:         return "unchanged";
        case SyncAction::Download:     return "download";
        case SyncAction::Upload:       return "upload";
        case SyncAction::DeleteLocal:  return "delete-local";
        case SyncAction::DeleteRemote: return "delete-remote";
        case SyncAction::Conflict:     return "conflict";
    }
    return "unchanged";
}

/* ------------------------------------------------------------------ *
 * FileMask                                                            *
 * ------------------------------------------------------------------ */

bool FileMask::Parse(const std::string & text, std::string & error)
{
    include.clear();
    exclude.clear();
    std::string trimmed = text;
    while (!trimmed.empty() && (trimmed[0] == ' ' || trimmed[0] == '"'))
    {
        trimmed.erase(0, 1);
    }
    while (!trimmed.empty() && (trimmed[trimmed.size() - 1] == ' ' ||
        trimmed[trimmed.size() - 1] == '"'))
    {
        trimmed.erase(trimmed.size() - 1);
    }
    if (trimmed.empty())
    {
        return true;
    }

    std::vector<std::string> groups;
    std::string current;
    for (size_t i = 0; i < trimmed.size(); i++)
    {
        if (trimmed[i] == '|')
        {
            groups.push_back(current);
            current.clear();
        }
        else
        {
            current += trimmed[i];
        }
    }
    groups.push_back(current);
    if (groups.size() > 2)
    {
        error = "A file mask has at most one '|' (include masks|exclude masks).";
        return false;
    }

    for (size_t g = 0; g < groups.size(); g++)
    {
        std::vector<std::string> & target = g == 0 ? include : exclude;
        std::string mask;
        for (size_t i = 0; i <= groups[g].size(); i++)
        {
            const bool end = i == groups[g].size();
            if (!end && groups[g][i] != ';')
            {
                mask += groups[g][i];
                continue;
            }
            if (!mask.empty())
            {
                if (mask.find('/') != std::string::npos ||
                    mask.find('\\') != std::string::npos)
                {
                    error = "The file mask '" + mask + "' contains a path "
                        "separator; only file name masks are supported.";
                    return false;
                }
                target.push_back(mask);
                mask.clear();
            }
        }
    }
    if (include.empty() && exclude.empty())
    {
        error = "The file mask '" + text + "' is empty.";
        return false;
    }
    return true;
}

bool FileMask::Accepts(const std::string & name, bool isDirectory) const
{
    for (size_t i = 0; i < exclude.size(); i++)
    {
        if (MatchesMask(exclude[i], name) ||
            (kIsWindows && MatchesMaskInsensitive(exclude[i], name)))
        {
            return false;
        }
    }
    /* An include mask never prunes a directory: the files below it may
     * still match, and pruning would silently skip them. */
    if (include.empty() || isDirectory)
    {
        return true;
    }
    for (size_t i = 0; i < include.size(); i++)
    {
        if (MatchesMask(include[i], name) ||
            (kIsWindows && MatchesMaskInsensitive(include[i], name)))
        {
            return true;
        }
    }
    return false;
}

std::string FileMask::Describe() const
{
    std::string result;
    for (size_t i = 0; i < include.size(); i++)
    {
        if (i != 0)
        {
            result += ";";
        }
        result += include[i];
    }
    if (!exclude.empty())
    {
        result += "|";
        for (size_t i = 0; i < exclude.size(); i++)
        {
            if (i != 0)
            {
                result += ";";
            }
            result += exclude[i];
        }
    }
    return result.empty() ? std::string("(none)") : result;
}

/* ------------------------------------------------------------------ *
 * SyncPlan                                                            *
 * ------------------------------------------------------------------ */

int SyncPlan::Count(SyncAction action) const
{
    int count = 0;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (items[i].action == action)
        {
            count++;
        }
    }
    return count;
}

unsigned long long SyncPlan::Bytes(SyncAction action) const
{
    unsigned long long total = 0;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (items[i].action != action || items[i].isDirectory)
        {
            continue;
        }
        total += action == SyncAction::Upload ? items[i].localSize : items[i].remoteSize;
    }
    return total;
}

bool SyncPlan::DeletesAnything() const
{
    return Count(SyncAction::DeleteLocal) != 0 ||
        Count(SyncAction::DeleteRemote) != 0;
}

namespace {

int ActionRank(SyncAction action)
{
    if (action == SyncAction::Upload || action == SyncAction::Download)
    {
        return 0;
    }
    if (action == SyncAction::Conflict)
    {
        return 1;
    }
    if (action == SyncAction::DeleteLocal || action == SyncAction::DeleteRemote)
    {
        return 2;
    }
    return 3; /* SyncAction::None: never interesting, sorted to the end */
}

int PathDepth(const std::string & path)
{
    int depth = 0;
    for (size_t i = 0; i < path.size(); i++)
    {
        if (path[i] == '/')
        {
            depth++;
        }
    }
    return depth;
}

bool ItemLess(const SyncItem & left, const SyncItem & right)
{
    const int rankLeft = ActionRank(left.action);
    const int rankRight = ActionRank(right.action);
    if (rankLeft != rankRight)
    {
        return rankLeft < rankRight;
    }
    if (rankLeft == 0 && left.isDirectory != right.isDirectory)
    {
        /* Directories before the files they contain. */
        return left.isDirectory;
    }
    if (rankLeft == 2)
    {
        /* Deletions deepest first, so that a directory is empty by the
         * time it is removed. */
        const int depthLeft = PathDepth(left.path);
        const int depthRight = PathDepth(right.path);
        if (depthLeft != depthRight)
        {
            return depthLeft > depthRight;
        }
    }
    return left.path < right.path;
}

} // namespace

void SyncPlan::Sort()
{
    std::stable_sort(items.begin(), items.end(), ItemLess);
}

/* ------------------------------------------------------------------ *
 * Paths                                                               *
 * ------------------------------------------------------------------ */

std::vector<std::string> SplitRelativePath(const std::string & path)
{
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < path.size(); i++)
    {
        if (path[i] == '/')
        {
            if (!current.empty())
            {
                parts.push_back(current);
                current.clear();
            }
        }
        else
        {
            current += path[i];
        }
    }
    if (!current.empty())
    {
        parts.push_back(current);
    }
    return parts;
}

std::string LocalPathForRelative(const std::string & root, const std::string & relative)
{
    std::string result = root;
    const std::vector<std::string> parts = SplitRelativePath(relative);
    for (size_t i = 0; i < parts.size(); i++)
    {
        result = JoinLocalPath(result, parts[i]);
    }
    return result;
}

std::string RemotePathForRelative(const std::string & root, const std::string & relative)
{
    std::string result = root;
    const std::vector<std::string> parts = SplitRelativePath(relative);
    for (size_t i = 0; i < parts.size(); i++)
    {
        result = JoinRemotePath(result, parts[i]);
    }
    return result;
}

/* ------------------------------------------------------------------ *
 * The planner                                                         *
 * ------------------------------------------------------------------ */

namespace {

class Planner
{
public:
    Planner(SyncFileSystem & localFs, SyncFileSystem & remoteFs,
        const std::string & localRoot, const std::string & remoteRoot,
        const SyncOptions & options, SyncPlan & plan) :
        mLocalFs(localFs), mRemoteFs(remoteFs), mLocalRoot(localRoot),
        mRemoteRoot(remoteRoot), mOptions(options), mPlan(plan)
    {
    }

    bool Run(std::string & error)
    {
        return PlanDirectory(std::string(), error);
    }

private:
    Planner(const Planner &);
    Planner & operator=(const Planner &);

    bool PlanDirectory(const std::string & relative, std::string & error)
    {
        std::string localError;
        std::string remoteError;
        std::vector<SyncEntry> localEntries;
        std::vector<SyncEntry> remoteEntries;
        const bool localOk = mLocalFs.ListDirectory(
            LocalPathForRelative(mLocalRoot, relative), localEntries, localError);
        const bool remoteOk = mRemoteFs.ListDirectory(
            RemotePathForRelative(mRemoteRoot, relative), remoteEntries, remoteError);

        if (!localOk && !remoteOk)
        {
            error = "Cannot read the local directory '" +
                LocalPathForRelative(mLocalRoot, relative) + "' (" + localError +
                ") or the remote directory '" +
                RemotePathForRelative(mRemoteRoot, relative) + "' (" + remoteError + ").";
            return false;
        }
        if (!localOk)
        {
            /* A missing local directory is created by a download, and is
             * an error for a synchronization that only uploads. */
            if (mOptions.mode == SyncMode::Remote)
            {
                SyncItem item;
                item.action = SyncAction::Upload;
                item.path = relative;
                item.isDirectory = true;
                item.reason = "the local directory does not exist yet";
                mPlan.items.push_back(item);
            }
            else if (!remoteEntries.empty())
            {
                AddSubtree(mRemoteFs, relative, SyncAction::Download,
                    RemotePathForRelative(mRemoteRoot, relative), error);
            }
            return error.empty();
        }
        if (!remoteOk)
        {
            if (mOptions.mode == SyncMode::Local)
            {
                SyncItem item;
                item.action = SyncAction::Download;
                item.path = relative;
                item.isDirectory = true;
                item.reason = "the remote directory does not exist yet";
                mPlan.items.push_back(item);
            }
            else if (!localEntries.empty())
            {
                AddSubtree(mLocalFs, relative, SyncAction::Upload,
                    LocalPathForRelative(mLocalRoot, relative), error);
            }
            return error.empty();
        }

        std::map<std::string, SyncEntry> localMap;
        std::map<std::string, SyncEntry> remoteMap;
        std::set<std::string> names;
        for (size_t i = 0; i < localEntries.size(); i++)
        {
            if (localEntries[i].name == "." || localEntries[i].name == "..")
            {
                continue;
            }
            localMap[localEntries[i].name] = localEntries[i];
            names.insert(localEntries[i].name);
        }
        for (size_t i = 0; i < remoteEntries.size(); i++)
        {
            if (remoteEntries[i].name == "." || remoteEntries[i].name == "..")
            {
                continue;
            }
            remoteMap[remoteEntries[i].name] = remoteEntries[i];
            names.insert(remoteEntries[i].name);
        }

        for (std::set<std::string>::const_iterator it = names.begin();
            it != names.end(); ++it)
        {
            const std::string & name = *it;
            std::map<std::string, SyncEntry>::const_iterator localIt = localMap.find(name);
            std::map<std::string, SyncEntry>::const_iterator remoteIt = remoteMap.find(name);
            const bool haveLocal = localIt != localMap.end();
            const bool haveRemote = remoteIt != remoteMap.end();
            const SyncEntry local = haveLocal ? localIt->second : SyncEntry();
            const SyncEntry remote = haveRemote ? remoteIt->second : SyncEntry();
            const bool isDirectory = (haveLocal && local.isDirectory) ||
                (haveRemote && remote.isDirectory);

            if (!mOptions.mask.Accepts(name, isDirectory))
            {
                continue;
            }

            const std::string child = relative.empty() ? name : relative + "/" + name;

            if (haveLocal && haveRemote && local.isDirectory == remote.isDirectory)
            {
                if (local.isDirectory)
                {
                    if (mOptions.recursive && !PlanDirectory(child, error))
                    {
                        return false;
                    }
                    continue;
                }
                CompareFile(child, local, remote);
                continue;
            }

            if (haveLocal && haveRemote)
            {
                SyncItem item;
                item.action = SyncAction::Conflict;
                item.path = child;
                item.isDirectory = isDirectory;
                item.localSize = local.size;
                item.remoteSize = remote.size;
                item.localTime = local.mtime;
                item.remoteTime = remote.mtime;
                item.reason = "it is a directory on one side and a file on the other";
                mPlan.items.push_back(item);
                continue;
            }

            /* Present on one side only. */
            if (haveLocal)
            {
                if (mOptions.mode == SyncMode::Local)
                {
                    AddObsolete(child, local, SyncAction::DeleteLocal, true);
                    continue;
                }
                if (mOptions.mode == SyncMode::Both && DeletedOnOtherSide(child, false))
                {
                    AddObsolete(child, local, SyncAction::DeleteLocal, false);
                    continue;
                }
                AddCopy(child, local, SyncAction::Upload,
                    mOptions.mode == SyncMode::Both ?
                        "it is new on the local side" : "it only exists locally",
                    LocalPathForRelative(mLocalRoot, child), error);
                continue;
            }

            if (mOptions.mode == SyncMode::Remote)
            {
                AddObsolete(child, remote, SyncAction::DeleteRemote, true);
                continue;
            }
            if (mOptions.mode == SyncMode::Both && DeletedOnOtherSide(child, true))
            {
                AddObsolete(child, remote, SyncAction::DeleteRemote, false);
                continue;
            }
            AddCopy(child, remote, SyncAction::Download,
                mOptions.mode == SyncMode::Both ?
                    "it is new on the remote side" : "it only exists remotely",
                RemotePathForRelative(mRemoteRoot, child), error);
        }
        return error.empty();
    }

    /* Adds a copy of 'entry' (a file) or of its whole subtree (a
     * directory) in the direction of 'action'. */
    void AddCopy(const std::string & relative, const SyncEntry & entry,
        SyncAction action, const std::string & reason, const std::string & sourcePath,
        std::string & error)
    {
        SyncItem item;
        item.action = action;
        item.path = relative;
        item.isDirectory = entry.isDirectory;
        item.localSize = entry.size;
        item.remoteSize = entry.size;
        item.localTime = entry.mtime;
        item.remoteTime = entry.mtime;
        item.reason = reason;
        mPlan.items.push_back(item);
        if (entry.isDirectory && mOptions.recursive)
        {
            SyncFileSystem & source = action == SyncAction::Upload ? mLocalFs : mRemoteFs;
            AddSubtree(source, relative, action, sourcePath, error);
        }
    }

    /* Copies every entry below a directory that exists on one side
     * only, so that a new subtree is planned as a whole. */
    void AddSubtree(SyncFileSystem & fs, const std::string & relative,
        SyncAction action, const std::string & path, std::string & error)
    {
        std::vector<SyncEntry> entries;
        std::string listError;
        if (!fs.ListDirectory(path, entries, listError))
        {
            return; /* unreadable subtree: reported when it is applied */
        }
        for (size_t i = 0; i < entries.size(); i++)
        {
            if (entries[i].name == "." || entries[i].name == "..")
            {
                continue;
            }
            const bool isDirectory = entries[i].isDirectory;
            if (!mOptions.mask.Accepts(entries[i].name, isDirectory))
            {
                continue;
            }
            const std::string child = relative.empty() ?
                entries[i].name : relative + "/" + entries[i].name;
            /* Without -recursive a directory is not descended into, and
             * an empty one is not created either. */
            if (isDirectory && !mOptions.recursive)
            {
                continue;
            }
            SyncItem item;
            item.action = action;
            item.path = child;
            item.isDirectory = isDirectory;
            item.localSize = entries[i].size;
            item.remoteSize = entries[i].size;
            item.localTime = entries[i].mtime;
            item.remoteTime = entries[i].mtime;
            item.reason = "part of a directory that exists on one side only";
            mPlan.items.push_back(item);
            if (isDirectory)
            {
                AddSubtree(fs, child, action,
                    action == SyncAction::Upload ?
                        LocalPathForRelative(mLocalRoot, child) :
                        RemotePathForRelative(mRemoteRoot, child), error);
            }
        }
    }

    /* Adds a deletion when the mode and the switches allow it.
     * 'mirror' is true for the one-directional modes, where the target
     * is made to match the source; in 'both' a deletion only happens
     * when the recorded state proves the entry was removed on the
     * other side. */
    void AddObsolete(const std::string & relative, const SyncEntry & entry,
        SyncAction action, bool mirror)
    {
        if (!mOptions.deleteObsolete)
        {
            return;
        }
        SyncItem item;
        item.action = action;
        item.path = relative;
        item.isDirectory = entry.isDirectory;
        item.localSize = entry.size;
        item.remoteSize = entry.size;
        item.localTime = entry.mtime;
        item.remoteTime = entry.mtime;
        item.reason = mirror ?
            "the other side does not have it" :
            "it was removed on the other side since the last synchronization";
        mPlan.items.push_back(item);
    }

    /* True when the recorded state says the entry used to exist on the
     * other side: it was deleted there, so deleting it here is the
     * synchronization and not a loss. */
    bool DeletedOnOtherSide(const std::string & relative, bool localIsMissing) const
    {
        if (mOptions.state == NULL || mOptions.state->Empty())
        {
            return false;
        }
        SyncStateEntry entry;
        if (!mOptions.state->Lookup(relative, entry))
        {
            return false;
        }
        return localIsMissing ? entry.localTime != 0 : entry.remoteTime != 0;
    }

    void CompareFile(const std::string & relative, const SyncEntry & local,
        const SyncEntry & remote)
    {
        SyncItem item;
        item.path = relative;
        item.isDirectory = false;
        item.localSize = local.size;
        item.remoteSize = remote.size;
        item.localTime = local.mtime;
        item.remoteTime = remote.mtime;

        if (mOptions.mode == SyncMode::Local)
        {
            if (Differs(remote, local))
            {
                item.action = SyncAction::Download;
                item.reason = Reason(remote, local, "remote", "local");
            }
            else
            {
                item.action = SyncAction::None;
            }
            mPlan.items.push_back(item);
            return;
        }
        if (mOptions.mode == SyncMode::Remote)
        {
            if (Differs(local, remote))
            {
                item.action = SyncAction::Upload;
                item.reason = Reason(local, remote, "local", "remote");
            }
            else
            {
                item.action = SyncAction::None;
            }
            mPlan.items.push_back(item);
            return;
        }

        /* Both directions. */
        SyncStateEntry recorded;
        const bool known = mOptions.state != NULL &&
            mOptions.state->Lookup(relative, recorded);
        if (!known)
        {
            if (local.mtime == remote.mtime && local.size == remote.size)
            {
                item.action = SyncAction::None;
            }
            else if (local.mtime == remote.mtime)
            {
                item.action = SyncAction::Conflict;
                item.reason = "the timestamps are equal but the sizes differ, and "
                    "there is no previous synchronization to compare with";
            }
            else if (local.mtime > remote.mtime)
            {
                item.action = SyncAction::Upload;
                item.reason = "the local file is newer and there is no previous "
                    "synchronization to compare with";
            }
            else
            {
                item.action = SyncAction::Download;
                item.reason = "the remote file is newer and there is no previous "
                    "synchronization to compare with";
            }
            mPlan.items.push_back(item);
            return;
        }

        const bool localChanged = local.mtime != recorded.localTime ||
            local.size != recorded.size;
        const bool remoteChanged = remote.mtime != recorded.remoteTime ||
            remote.size != recorded.size;
        if (localChanged && remoteChanged)
        {
            item.action = SyncAction::Conflict;
            item.reason = "it changed on both sides since the last synchronization";
        }
        else if (localChanged)
        {
            item.action = SyncAction::Upload;
            item.reason = "it changed locally since the last synchronization";
        }
        else if (remoteChanged)
        {
            item.action = SyncAction::Download;
            item.reason = "it changed remotely since the last synchronization";
        }
        else
        {
            item.action = SyncAction::None;
        }
        mPlan.items.push_back(item);
    }

    bool Differs(const SyncEntry & source, const SyncEntry & target) const
    {
        switch (mOptions.criteria)
        {
            case SyncCriteria::Time:
                return source.mtime > target.mtime;
            case SyncCriteria::Size:
                return source.size != target.size;
            case SyncCriteria::Either:
                return source.mtime != target.mtime || source.size != target.size;
            case SyncCriteria::None:
                return true;
        }
        return false;
    }

    static std::string Reason(const SyncEntry & source, const SyncEntry & target,
        const char * sourceLabel, const char * targetLabel)
    {
        std::string reason = std::string("the ") + sourceLabel + " file is newer";
        if (source.size != target.size)
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), " (%llu against %llu bytes)",
                static_cast<unsigned long long>(source.size),
                static_cast<unsigned long long>(target.size));
            reason += buffer;
        }
        (void)targetLabel;
        return reason;
    }

    SyncFileSystem & mLocalFs;
    SyncFileSystem & mRemoteFs;
    std::string mLocalRoot;
    std::string mRemoteRoot;
    const SyncOptions & mOptions;
    SyncPlan & mPlan;
};

} // namespace

bool PlanSynchronization(SyncFileSystem & localFs, SyncFileSystem & remoteFs,
    const std::string & localRoot, const std::string & remoteRoot,
    const SyncOptions & options, SyncPlan & plan, std::string & error)
{
    if (localRoot.empty())
    {
        error = "The local directory of a synchronization is empty.";
        return false;
    }
    if (remoteRoot.empty())
    {
        error = "The remote directory of a synchronization is empty.";
        return false;
    }
    plan = SyncPlan();
    plan.mode = options.mode;
    plan.criteria = options.criteria;
    plan.mask = options.mask;
    plan.localRoot = localRoot;
    plan.remoteRoot = remoteRoot;

    Planner planner(localFs, remoteFs, localRoot, remoteRoot, options, plan);
    if (!planner.Run(error))
    {
        return false;
    }
    plan.Sort();
    return true;
}

/* ------------------------------------------------------------------ *
 * SyncState                                                           *
 * ------------------------------------------------------------------ */

namespace {

/* FNV-1a, only to name the state file after the pair of directories. */
std::string HashText(const std::string & text)
{
    unsigned long long hash = 1469598103934665603ULL;
    for (size_t i = 0; i < text.size(); i++)
    {
        hash ^= static_cast<unsigned char>(text[i]);
        hash *= 1099511628211ULL;
    }
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%016llx", hash);
    return std::string(buffer);
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

} // namespace

std::string SyncState::MakeKey(const std::string & host,
    const std::string & localRoot, const std::string & remoteRoot)
{
    return host + "|" + localRoot + "|" + remoteRoot;
}

std::string SyncState::DefaultPath(const std::string & key)
{
    const std::string config = ConfigDirectory();
    if (config.empty())
    {
        return std::string();
    }
    return config + "/sync/" + HashText(key) + ".state";
}

bool SyncState::Lookup(const std::string & relativePath, SyncStateEntry & entry) const
{
    std::map<std::string, SyncStateEntry>::const_iterator it =
        mEntries.find(relativePath);
    if (it == mEntries.end())
    {
        return false;
    }
    entry = it->second;
    return true;
}

void SyncState::Update(const std::string & relativePath, const SyncStateEntry & entry)
{
    mEntries[relativePath] = entry;
}

void SyncState::Remove(const std::string & relativePath)
{
    mEntries.erase(relativePath);
}

bool SyncState::Load(const std::string & path, std::string & error)
{
    mEntries.clear();
    if (path.empty())
    {
        return true; /* no configuration directory: a stateless comparison */
    }
    FILE * file = fopen(path.c_str(), "rb");
    if (file == NULL)
    {
        return true; /* no previous synchronization, not an error */
    }
    char buffer[8192];
    while (fgets(buffer, sizeof(buffer), file) != NULL)
    {
        std::string line(buffer);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
        {
            line.erase(line.size() - 1);
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::vector<std::string> fields;
        std::string current;
        for (size_t i = 0; i < line.size(); i++)
        {
            if (line[i] == '\t')
            {
                fields.push_back(current);
                current.clear();
            }
            else
            {
                current += line[i];
            }
        }
        fields.push_back(current);
        if (fields.size() < 4)
        {
            continue;
        }
        SyncStateEntry entry;
        entry.localTime = static_cast<unsigned long>(strtoul(fields[1].c_str(), NULL, 10));
        entry.remoteTime = static_cast<unsigned long>(strtoul(fields[2].c_str(), NULL, 10));
        entry.size = strtoull(fields[3].c_str(), NULL, 10);
        mEntries[UnescapeValue(fields[0])] = entry;
    }
    fclose(file);
    (void)error;
    return true;
}

bool SyncState::Save(const std::string & path, std::string & error) const
{
    if (path.empty())
    {
        return true; /* nothing to remember, and not a failure */
    }
    const std::string directory = path.substr(0, path.find_last_of('/'));
    if (!directory.empty() && directory != path)
    {
        std::string createError;
        if (!CreatePrivateDirectory(directory, createError))
        {
            error = createError;
            return false;
        }
    }
    FILE * file = fopen(path.c_str(), "wb");
    if (file == NULL)
    {
        error = std::string("Cannot write the synchronization state '") + path + "'.";
        return false;
    }
    fprintf(file, "# LinuxSCP synchronization state, written %llu\n", NowSeconds());
    for (std::map<std::string, SyncStateEntry>::const_iterator it = mEntries.begin();
        it != mEntries.end(); ++it)
    {
        fprintf(file, "%s\t%lu\t%lu\t%llu\n", EscapeValue(it->first).c_str(),
            it->second.localTime, it->second.remoteTime, it->second.size);
    }
    fclose(file);
    return true;
}

} // namespace linuxscp
