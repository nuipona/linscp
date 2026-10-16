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

namespace linuxscp {

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

} // namespace linuxscp
