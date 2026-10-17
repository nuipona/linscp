/*
 * ListingParse: see ListingParse.h. These helpers were the private
 * parsing half of the curl backend and are now shared with the native
 * FTP engine, so that a listing from LIST and one from MLSD produce the
 * same entries.
 */

#include "ListingParse.h"

#include <cstring>
#include <sstream>

#include "Platform.h"

namespace linscp {

ListingEntry::ListingEntry()
    : typeKnown(false), isDir(false), permissions(0100644), size(0),
      timeKnown(false), mtime(0)
{
}

bool SplitLongListing(const std::string & line, std::vector<std::string> & fields)
{
    fields.clear();
    std::istringstream stream(line);
    std::string field;
    while (fields.size() < 8 && (stream >> field))
    {
        fields.push_back(field);
    }
    if (fields.size() < 8)
    {
        return false;
    }
    std::string rest;
    std::getline(stream, rest);
    size_t first = rest.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return false;
    }
    fields.push_back(rest.substr(first));
    return true;
}

bool LooksLikePermissions(const std::string & text)
{
    if (text.size() != 10)
    {
        return false;
    }
    const char types[] = "-dlbcps";
    if (strchr(types, text[0]) == NULL)
    {
        return false;
    }
    for (size_t i = 1; i < 10; i++)
    {
        if (strchr("rwxstST-", text[i]) == NULL)
        {
            return false;
        }
    }
    return true;
}

unsigned long PermissionsFromString(const std::string & text)
{
    unsigned long mode = text[0] == 'd' ? 0040000UL : 0100000UL;
    static const unsigned long bits[9] = {
        0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001
    };
    for (size_t i = 0; i < 9 && i + 1 < text.size(); i++)
    {
        char c = text[i + 1];
        if (c == 'r' || c == 'w' || c == 'x' || c == 's' || c == 't')
        {
            mode |= bits[i];
        }
    }
    return mode;
}

bool ParseUnixListingLine(const std::string & line, ListingEntry & entry)
{
    std::vector<std::string> fields;
    if (!SplitLongListing(line, fields) || !LooksLikePermissions(fields[0]))
    {
        return false;
    }
    entry.name = fields[8];
    entry.typeKnown = true;
    entry.isDir = fields[0][0] == 'd';
    entry.permissions = PermissionsFromString(fields[0]);
    try
    {
        entry.size = std::stoull(fields[4]);
    }
    catch (...)
    {
        entry.size = 0;
    }
    entry.mtime = ParseListingDate(fields[5], fields[6], fields[7]);
    entry.timeKnown = entry.mtime != 0;
    return true;
}

std::string PermissionsString(unsigned long permissions)
{
    const char type =
        (permissions & 0170000UL) == 0040000UL ? 'd' :
        (permissions & 0170000UL) == 0120000UL ? 'l' :
        (permissions & 0170000UL) == 0020000UL ? 'c' :
        (permissions & 0170000UL) == 0060000UL ? 'b' :
        (permissions & 0170000UL) == 0010000UL ? 'p' :
        (permissions & 0170000UL) == 0140000UL ? 's' : '-';
    std::string text(1, type);
    static const char rwx[9] = { 'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x' };
    static const unsigned long bits[9] = {
        0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001
    };
    for (size_t i = 0; i < 9; i++)
    {
        text += (permissions & bits[i]) != 0 ? rwx[i] : '-';
    }
    if (permissions & 04000UL) { text[3] = text[3] == 'x' ? 's' : 'S'; }
    if (permissions & 02000UL) { text[6] = text[6] == 'x' ? 's' : 'S'; }
    if (permissions & 01000UL) { text[9] = text[9] == 'x' ? 't' : 'T'; }
    return text;
}

/* ---- DOS/IIS listing parser ---------------------------------------------
 *
 * IIS and other DOS-syntactic FTP servers emit:
 *
 *   03-25-23  11:22AM       <DIR>          dirname
 *   07-14-22  04:00PM             12345    filename
 *
 * Date is MM-DD-YY or MM-DD-YYYY, time is HH:MMAM/PM, <DIR> marks a
 * directory, otherwise the size follows, then the name.
 */

bool LooksLikeDosListing(const std::string & line)
{
    /* A DOS date starts with a two-digit month, a dash, two-digit day,
     * a dash and a two- or four-digit year.  Accept either separator
     * (- or /).  The check is deliberately narrow to avoid false
     * positives on arbitrary text. */
    if (line.size() < 18)
    {
        return false;
    }
    char c0 = line[0], c1 = line[1];
    if (c0 < '0' || c0 > '9' || c1 < '0' || c1 > '9')
    {
        return false;
    }
    if (line[2] != '-' && line[2] != '/')
    {
        return false;
    }
    /* Day: two digits */
    char c3 = line[3], c4 = line[4];
    if (c3 < '0' || c3 > '9' || c4 < '0' || c4 > '9')
    {
        return false;
    }
    if (line[5] != '-' && line[5] != '/')
    {
        return false;
    }
    /* Year: two or four digits */
    for (size_t i = 6; i < 10 && i < line.size(); i++)
    {
        if (line[i] < '0' || line[i] > '9')
        {
            /* Accept two-digit year followed by whitespace */
            if (i == 8 && (line[i] == ' ' || line[i] == '\t'))
            {
                break;
            }
            return false;
        }
    }
    return true;
}

namespace {

bool ParseDosDate(const std::string & dateStr, const std::string & timeStr,
    unsigned long & mtime)
{
    /* dateStr: MM-DD-YY or MM-DD-YYYY  timeStr: HH:MMAM or HH:MM PM */
    if (dateStr.size() < 8)
    {
        return false;
    }
    int month = 0, day = 0, year = 0;
    /* Parse month-day-year */
    const char * ds = dateStr.c_str();
    month = (ds[0] - '0') * 10 + (ds[1] - '0');
    day = (ds[3] - '0') * 10 + (ds[4] - '0');
    if (dateStr.size() == 10)
    {
        year = (ds[6] - '0') * 1000 + (ds[7] - '0') * 100 +
               (ds[8] - '0') * 10 + (ds[9] - '0');
    }
    else if (dateStr.size() == 8)
    {
        year = (ds[6] - '0') * 10 + (ds[7] - '0');
        year += (year < 70) ? 2000 : 1900;
    }
    else
    {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31)
    {
        return false;
    }

    /* Parse time: HH:MMAM or HH:MM PM or 24-hour */
    int hour = 0, minute = 0;
    bool isPM = false;
    const char * ts = timeStr.c_str();
    if (timeStr.size() >= 4)
    {
        hour = (ts[0] - '0') * 10 + (ts[1] - '0');
        if (ts[2] == ':')
        {
            minute = (ts[3] - '0') * 10;
            if (timeStr.size() > 4 && ts[4] >= '0' && ts[4] <= '9')
            {
                minute += ts[4] - '0';
            }
        }
        /* Look for AM/PM */
        size_t ampm = timeStr.find("PM");
        if (ampm == std::string::npos)
        {
            ampm = timeStr.find("pm");
        }
        if (ampm != std::string::npos)
        {
            isPM = true;
        }
        if (isPM && hour < 12) hour += 12;
        if (!isPM && hour == 12) hour = 0;
    }

    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == static_cast<time_t>(-1))
    {
        return false;
    }
    mtime = static_cast<unsigned long>(t);
    return true;
}

} // namespace

bool ParseDosListingLine(const std::string & line, ListingEntry & entry)
{
    if (!LooksLikeDosListing(line))
    {
        return false;
    }
    std::istringstream stream(line);
    std::string dateStr, timeStr, dirOrSize;
    if (!(stream >> dateStr >> timeStr >> dirOrSize))
    {
        return false;
    }

    /* The name is everything after the size/DIR field, trimmed. */
    std::string rest;
    std::getline(stream, rest);
    size_t first = rest.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return false;
    }
    std::string name = rest.substr(first);
    /* Trim trailing whitespace */
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' ||
           name.back() == '\r' || name.back() == '\n'))
    {
        name.pop_back();
    }
    if (name.empty() || name == "." || name == "..")
    {
        return false;
    }

    entry.name = name;
    entry.typeKnown = true;

    if (dirOrSize == "<DIR>" || dirOrSize == "<dir>")
    {
        entry.isDir = true;
        entry.permissions = 0040755UL;
        entry.size = 0;
    }
    else
    {
        entry.isDir = false;
        entry.permissions = 0100644UL;
        try
        {
            entry.size = std::stoull(dirOrSize);
        }
        catch (...)
        {
            entry.size = 0;
        }
    }

    entry.longname = line;
    entry.timeKnown = ParseDosDate(dateStr, timeStr, entry.mtime);
    return true;
}

/* ---- VMS (OpenVMS) listing parser ----------------------------------------
 *
 * VMS FTP servers emit:
 *
 *   FILENAME.TXT;1    25  25-JUN-2023 11:22:33 [OWNER] (RWED,RWED,RE,RE)
 *   SUBDIR.DIR;1       3  25-JUN-2023 11:22:33 [OWNER] (RWED,RWED,RE,RE)
 *
 * The version (;N) follows the extension.  The name is everything before
 * the first semicolon.  A .DIR extension marks a directory.
 */

bool LooksLikeVmsListing(const std::string & line)
{
    /* A VMS listing has a semicolon-delimited version, a numeric size,
     * a date in DD-MON-YYYY format and an owner in brackets.  At minimum
     * it needs the semicolon, a space, digits, a space and a date-like
     * token. */
    if (line.empty() || line[0] == ' ')
    {
        return false;
    }
    size_t semi = line.find(';');
    if (semi == std::string::npos || semi < 2)
    {
        return false;
    }
    /* After the semicolon there must be a version number, then whitespace
     * and a size.  Be conservative: look for a DD-MON-YYYY date pattern
     * further along. */
    size_t dash = line.find('-', semi + 1);
    if (dash == std::string::npos || dash + 11 > line.size())
    {
        return false;
    }
    /* The month abbreviation after the dash */
    static const char * months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    bool foundMonth = false;
    std::string after = line.substr(dash + 1, 3);
    for (int i = 0; i < 12; i++)
    {
        if (after == months[i])
        {
            foundMonth = true;
            break;
        }
    }
    if (!foundMonth)
    {
        return false;
    }
    /* Look for the bracketed owner */
    return line.find('[') != std::string::npos &&
           line.find(']') != std::string::npos;
}

namespace {

int VmsMonthFromText(const std::string & text)
{
    static const char * months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    std::string upper = text;
    for (size_t i = 0; i < upper.size(); i++)
    {
        if (upper[i] >= 'a' && upper[i] <= 'z')
        {
            upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
        }
    }
    for (int i = 0; i < 12; i++)
    {
        if (upper == months[i])
        {
            return i;
        }
    }
    return -1;
}

} // namespace

bool ParseVmsListingLine(const std::string & line, ListingEntry & entry)
{
    if (!LooksLikeVmsListing(line))
    {
        return false;
    }

    /* Split on semicolon to get the version, then parse name.ext;N SIZE
     * DD-MON-YYYY HH:MM:SS [OWNER] (perms) */
    size_t semi = line.find(';');
    /* The version number after the semicolon */
    size_t versionEnd = line.find_first_of(" \t", semi + 1);
    if (versionEnd == std::string::npos)
    {
        return false;
    }

    /* The name with extension is everything before the semicolon. */
    std::string nameWithExt = line.substr(0, semi);

    /* Skip the version and read the size. */
    size_t pos = versionEnd + 1;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
    {
        pos++;
    }
    size_t sizeStart = pos;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9')
    {
        pos++;
    }
    if (pos == sizeStart)
    {
        return false;
    }
    unsigned long long size = 0;
    try
    {
        size = std::stoull(line.substr(sizeStart, pos - sizeStart));
    }
    catch (...)
    {
        size = 0;
    }

    /* Skip whitespace, then read the date: DD-MON-YYYY */
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
    {
        pos++;
    }
    size_t dateStart = pos;
    /* Find the space before the time */
    size_t dateEnd = line.find(' ', pos);
    if (dateEnd == std::string::npos || dateEnd < dateStart + 11)
    {
        return false;
    }
    std::string datePart = line.substr(dateStart, dateEnd - dateStart);

    /* Parse DD-MON-YYYY */
    int day = 0, month = 0, year = 0;
    size_t d1 = datePart.find('-');
    if (d1 == std::string::npos || d1 < 1 || d1 > 2)
    {
        return false;
    }
    day = 0;
    for (size_t i = 0; i < d1; i++)
    {
        day = day * 10 + (datePart[i] - '0');
    }
    size_t d2 = datePart.find('-', d1 + 1);
    if (d2 == std::string::npos)
    {
        return false;
    }
    month = VmsMonthFromText(datePart.substr(d1 + 1, d2 - d1 - 1));
    if (month < 0)
    {
        return false;
    }
    year = 0;
    for (size_t i = d2 + 1; i < datePart.size(); i++)
    {
        if (datePart[i] >= '0' && datePart[i] <= '9')
        {
            year = year * 10 + (datePart[i] - '0');
        }
    }

    /* Parse time HH:MM:SS */
    pos = dateEnd + 1;
    int hour = 0, minute = 0;
    if (pos + 7 < line.size())
    {
        hour = (line[pos] - '0') * 10 + (line[pos + 1] - '0');
        minute = (line[pos + 3] - '0') * 10 + (line[pos + 4] - '0');
    }

    unsigned long mtime = 0;
    struct tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t != static_cast<time_t>(-1))
    {
        mtime = static_cast<unsigned long>(t);
    }

    /* Determine whether it is a directory (.DIR extension). */
    bool isDir = false;
    std::string name = nameWithExt;
    std::string upperExt;
    size_t dot = nameWithExt.rfind('.');
    if (dot != std::string::npos)
    {
        upperExt = nameWithExt.substr(dot + 1);
        for (size_t i = 0; i < upperExt.size(); i++)
        {
            if (upperExt[i] >= 'a' && upperExt[i] <= 'z')
            {
                upperExt[i] = static_cast<char>(upperExt[i] - 'a' + 'A');
            }
        }
        if (upperExt == "DIR")
        {
            isDir = true;
            name = nameWithExt.substr(0, dot);
        }
    }

    if (name.empty() || name == "." || name == "..")
    {
        return false;
    }

    entry.name = name;
    entry.typeKnown = true;
    entry.isDir = isDir;
    entry.permissions = isDir ? 0040755UL : 0100644UL;
    entry.size = isDir ? 0 : size;
    entry.timeKnown = mtime != 0;
    entry.mtime = mtime;
    entry.longname = line;
    return true;
}

bool ParseAnyListingLine(const std::string & line, ListingEntry & entry)
{
    if (ParseUnixListingLine(line, entry))
    {
        return true;
    }
    if (ParseDosListingLine(line, entry))
    {
        return true;
    }
    if (ParseVmsListingLine(line, entry))
    {
        return true;
    }
    return false;
}

} // namespace linscp
