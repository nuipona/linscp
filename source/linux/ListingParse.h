/*
 * ListingParse: the directory-entry type and the listing lines the
 * protocols hand back.
 *
 * There are three producers and they must agree, because the console
 * prints one listing format and the synchronizer compares one entry type:
 *
 *   - the curl backend, whose FTPS/SCP/HTTP listings are text,
 *   - the native FTP engine (FtpClient.cpp), which gets the same text
 *     from LIST and structured facts from MLSD/MLST,
 *   - the WebDAV backend, which builds entries from PROPFIND properties.
 *
 * A names-only listing leaves 'typeKnown' false: neither the type nor the
 * size of the entry is known then, and a caller that needs to know has to
 * ask the server something else.
 */

#ifndef LINUXSCP_LISTINGPARSE_H
#define LINUXSCP_LISTINGPARSE_H

#include <string>
#include <vector>

namespace linuxscp {

struct ListingEntry
{
    std::string name;
    std::string longname;
    /* False for a names-only listing, where neither the type nor the
     * size of the entry is known. */
    bool typeKnown;
    bool isDir;
    unsigned long permissions;
    unsigned long long size;
    /* Set when the listing carried a usable date column. */
    bool timeKnown;
    unsigned long mtime;

    ListingEntry();
};

/* Splits an 'ls -l'-style line into exactly 9 fields (the 9th keeps any
 * spaces of the file name). Returns false for bare-name listings. */
bool SplitLongListing(const std::string & line, std::vector<std::string> & fields);

/* True when the first field of a listing line is a mode column. */
bool LooksLikePermissions(const std::string & text);

/* 'drwxr-xr-x' -> 0040755 (the type bits are included, so the result can
 * be passed to the SFTP attribute conversion unchanged). */
unsigned long PermissionsFromString(const std::string & text);

/*
 * One line of an 'ls -l'-style listing (what LIST returns) as an entry.
 * Returns false when the line is not a long listing at all, in which case
 * the caller may still treat the line as a bare file name.
 */
bool ParseUnixListingLine(const std::string & line, ListingEntry & entry);

/* Formats mode bits the way 'ls -l' does ('-rw-r--r--'), which is what
 * the console prints for a long listing. */
std::string PermissionsString(unsigned long permissions);

/*
 * Non-Unix listing formats: FTP servers that do not speak Unix may return
 * DOS (IIS) or VMS (OpenVMS) directory listings from LIST.  MLSD provides
 * a structured alternative, but a server that refuses MLSD can only fall
 * back to its own LIST dialect.  The parser decides which dialect to try
 * and reports what it parsed, so the caller never treats an unparsed
 * directory as empty.
 */

/* True when 'line' looks like a DOS/IIS listing:
 *   03-25-23  11:22AM       <DIR>          dirname
 *   07-14-22  04:00PM             12345    filename  */
bool LooksLikeDosListing(const std::string & line);

/* Parses one DOS/IIS listing line.  Returns false when 'line' is not a
 * recognizable DOS listing entry (the caller may still try the next
 * parser). */
bool ParseDosListingLine(const std::string & line, ListingEntry & entry);

/* True when 'line' looks like a VMS (Multinet/UCX) listing:
 *   filename.ext;1    25  25-JUN-2023 11:22:33 [OWNER] (RWED,RWED,RE,RE)
 *   DIR.DIR;1         3   25-JUN-2023 11:22:33 [OWNER] (RWED,RWED,RE,RE)  */
bool LooksLikeVmsListing(const std::string & line);

/* Parses one VMS listing line.  The version number (;N) is stripped from
 * the name.  Returns false when 'line' is not a recognizable VMS entry. */
bool ParseVmsListingLine(const std::string & line, ListingEntry & entry);

/* Unified listing parser: tries Unix, then DOS, then VMS and returns the
 * first that matches.  A caller that already tried one dialect can skip
 * it by checking its LooksLike* first. */
bool ParseAnyListingLine(const std::string & line, ListingEntry & entry);

} // namespace linuxscp

#endif /* LINUXSCP_LISTINGPARSE_H */
