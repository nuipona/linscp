/*
 * SyncPlanner: compares two directory trees and turns the differences
 * into an explicit, reviewable plan.
 *
 * The planner never touches the file systems: it only reads directory
 * listings through the SyncFileSystem interface and returns a SyncPlan
 * describing what a synchronization would do. The caller decides
 * whether to print the plan ('synchronize -preview'), apply it, or ask
 * first. That is what makes '-delete' and bidirectional ('both')
 * synchronization safe: the destructive part is a line in a plan that
 * can be reviewed, not a side effect of walking a directory.
 *
 * Both sides are reached through the same interface, so the local
 * directory and the remote directory (native SFTP or a curl backend)
 * are compared by identical rules, and the planner can be tested with
 * two in-memory file systems.
 *
 * SyncState is the memory of a previous synchronization of the same
 * pair of directories. It is what turns 'both' from two sequential
 * copies into conflict detection: an entry that changed on both sides
 * since the last pass is reported as a conflict instead of being
 * silently overwritten in one direction.
 */

#ifndef LINUXSCP_SYNCPLANNER_H
#define LINUXSCP_SYNCPLANNER_H

#include <map>
#include <string>
#include <vector>

namespace linuxscp {

/* Which side is the target of the synchronization.
 *
 *   Local  - the remote tree wins: changes are downloaded.
 *   Remote - the local tree wins: changes are uploaded.
 *   Both   - whichever side changed since the last synchronization
 *            wins; a change on both sides is a conflict. */
enum class SyncMode
{
    Local,
    Remote,
    Both
};

/* What counts as a difference. */
enum class SyncCriteria
{
    Time,    /* the source timestamp is newer than the target's */
    Size,    /* the sizes differ (regardless of the timestamps) */
    Either,  /* the timestamps or the sizes differ */
    None     /* always copy */
};

const char * SyncModeName(SyncMode mode);
const char * SyncCriteriaName(SyncCriteria criteria);

/* A set of include/exclude file masks.
 *
 * The syntax is the WinSCP one, restricted to file name masks:
 *
 *   "*.txt"                 include only files matching *.txt
 *   "|*.bak"                exclude files matching *.bak
 *   "*.txt|*.bak;*.tmp"     include *.txt except *.bak and *.tmp
 *
 * Several masks are separated by ';'. A mask without wildcards matches
 * that exact name. Exclusions are also applied to directories, which
 * prunes a whole subtree; an include mask never prunes a directory,
 * because the files below it may still match. */
struct FileMask
{
    std::vector<std::string> include;
    std::vector<std::string> exclude;

    bool empty() const { return include.empty() && exclude.empty(); }
    bool Parse(const std::string & text, std::string & error);
    bool Accepts(const std::string & name, bool isDirectory) const;
    std::string Describe() const;
};

/* One directory entry, as the planner sees it. */
struct SyncEntry
{
    std::string name;
    bool isDirectory;
    unsigned long long size;
    unsigned long mtime;      /* seconds since the epoch, 0 when unknown */
    unsigned long permissions;

    SyncEntry() : isDirectory(false), size(0), mtime(0), permissions(0) {}
};

/* Read-only view of one side of a synchronization. */
class SyncFileSystem
{
public:
    virtual ~SyncFileSystem() {}
    /* Entries of 'path' (without '.' and '..'), or false with 'error'. */
    virtual bool ListDirectory(const std::string & path,
        std::vector<SyncEntry> & entries, std::string & error) = 0;
    /* "local" or "remote": used in messages only. */
    virtual const char * Label() const = 0;
};

enum class SyncAction
{
    None,          /* both sides already agree */
    Download,      /* remote -> local */
    Upload,        /* local -> remote */
    DeleteLocal,   /* obsolete: it is gone from the remote side */
    DeleteRemote,  /* obsolete: it is gone from the local side */
    Conflict       /* changed on both sides; needs a decision */
};

const char * SyncActionName(SyncAction action);

/* One line of a plan: what to do with one path.
 *
 * 'path' is relative to the two synchronized roots and always uses '/',
 * so that it identifies the same entry on both sides no matter what the
 * local separator is. */
struct SyncItem
{
    SyncAction action;
    std::string path;
    bool isDirectory;
    unsigned long long localSize;
    unsigned long long remoteSize;
    unsigned long localTime;
    unsigned long remoteTime;
    std::string reason;    /* why this action was chosen */

    SyncItem() : action(SyncAction::None), isDirectory(false), localSize(0),
        remoteSize(0), localTime(0), remoteTime(0) {}
};

struct SyncPlan
{
    std::vector<SyncItem> items;
    SyncMode mode;
    SyncCriteria criteria;
    FileMask mask;
    std::string localRoot;
    std::string remoteRoot;

    /* Number of items with the given action. */
    int Count(SyncAction action) const;
    /* Bytes that the transfers of the given action would move. */
    unsigned long long Bytes(SyncAction action) const;
    /* True when applying the plan removes anything. */
    bool DeletesAnything() const;
    /* True when the plan needs a decision before it can be applied. */
    bool HasConflicts() const { return Count(SyncAction::Conflict) != 0; }
    /* Orders the items so that they can be applied in one pass:
     * directories before the files in them, transfers before deletions
     * and deletions from the deepest path upwards. */
    void Sort();
};

/* Everything the planner needs to know beyond the two roots. */
struct SyncOptions
{
    SyncMode mode;
    SyncCriteria criteria;
    FileMask mask;
    bool deleteObsolete;
    bool recursive;
    /* Memory of the previous pass, or NULL for a stateless comparison.
     * 'both' only detects conflicts when there is a state, and only
     * deletes when the state proves an entry was removed on the other
     * side. */
    class SyncState * state;

    SyncOptions() : mode(SyncMode::Local), criteria(SyncCriteria::Time),
        deleteObsolete(false), recursive(true), state(NULL) {}
};

bool PlanSynchronization(SyncFileSystem & localFs, SyncFileSystem & remoteFs,
    const std::string & localRoot, const std::string & remoteRoot,
    const SyncOptions & options, SyncPlan & plan, std::string & error);

/* Splits a relative plan path ('a/b/c') into its components. */
std::vector<std::string> SplitRelativePath(const std::string & path);
/* Joins the components of a relative plan path with the local
 * separator, after 'root'. */
std::string LocalPathForRelative(const std::string & root, const std::string & relative);
/* Joins the components of a relative plan path with '/', after 'root'. */
std::string RemotePathForRelative(const std::string & root, const std::string & relative);

/* ------------------------------------------------------------------ *
 * SyncState: the outcome of the previous synchronization of one pair  *
 * of directories, used for conflict detection and safe deletion.      *
 * ------------------------------------------------------------------ */

struct SyncStateEntry
{
    unsigned long localTime;
    unsigned long remoteTime;
    unsigned long long size;

    SyncStateEntry() : localTime(0), remoteTime(0), size(0) {}
};

class SyncState
{
public:
    /* Identifies one pair of directories, including the server, so that
     * two sessions to the same host and path share their history and
     * different servers never do. */
    static std::string MakeKey(const std::string & host,
        const std::string & localRoot, const std::string & remoteRoot);
    /* File holding the state of that key, inside the configuration
     * directory. Empty when there is no configuration directory. */
    static std::string DefaultPath(const std::string & key);

    bool Load(const std::string & path, std::string & error);
    bool Save(const std::string & path, std::string & error) const;
    void Clear() { mEntries.clear(); }
    size_t Size() const { return mEntries.size(); }
    bool Empty() const { return mEntries.empty(); }

    bool Lookup(const std::string & relativePath, SyncStateEntry & entry) const;
    void Update(const std::string & relativePath, const SyncStateEntry & entry);
    void Remove(const std::string & relativePath);

private:
    std::map<std::string, SyncStateEntry> mEntries;
};

} // namespace linuxscp

#endif /* LINUXSCP_SYNCPLANNER_H */
