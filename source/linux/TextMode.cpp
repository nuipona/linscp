/*
 * TextMode: see TextMode.h. The conversion works in fixed-size chunks
 * and never loads a whole file, so a large text file costs no more
 * memory than one chunk.
 */

#include "TextMode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "PathUtil.h"
#include "Platform.h"

namespace linuxscp {

const char * const kAsciiFileMask =
    "*.txt;*.htm;*.html;*.php;*.php3;*.cgi;*.c;*.cpp;*.h;*.pas;*.bas;*.tex;"
    "*.pl;*.js;*.htaccess;*.xhtml;*.css;*.ini;*.inf;*.bat;*.cmd;*.csv;*.log;"
    "*.md;*.json;*.xml;*.yml;*.yaml;*.py;*.rb;*.sh;*.conf;*.sql;*.srt;*.vtt";

const char * TransferModeName(TransferMode mode)
{
    switch (mode)
    {
        case TransferMode::Binary:    return "binary";
        case TransferMode::Ascii:     return "ascii";
        case TransferMode::Automatic: return "automatic";
    }
    return "binary";
}

bool ParseTransferMode(const std::string & text, TransferMode & mode)
{
    std::string lowered;
    for (size_t i = 0; i < text.size(); i++)
    {
        lowered += static_cast<char>(tolower(static_cast<unsigned char>(text[i])));
    }
    if (lowered == "binary")
    {
        mode = TransferMode::Binary;
        return true;
    }
    if (lowered == "ascii")
    {
        mode = TransferMode::Ascii;
        return true;
    }
    if (lowered == "automatic")
    {
        mode = TransferMode::Automatic;
        return true;
    }
    return false;
}

bool IsAsciiFileName(const std::string & name)
{
    std::vector<std::string> masks;
    const std::string list(kAsciiFileMask);
    std::string current;
    for (size_t i = 0; i <= list.size(); i++)
    {
        if (i < list.size() && list[i] != ';')
        {
            current += list[i];
            continue;
        }
        if (!current.empty())
        {
            masks.push_back(current);
        }
        current.clear();
    }
    const std::string leaf = name.find('/') != std::string::npos ?
        RemoteFileName(name) : LocalFileName(name);
    for (size_t i = 0; i < masks.size(); i++)
    {
        if (MatchesMask(masks[i], leaf) ||
            (kIsWindows && MatchesMaskInsensitive(masks[i], leaf)))
        {
            return true;
        }
    }
    return false;
}

bool WantsAsciiConversion(TransferMode mode, const std::string & name)
{
    switch (mode)
    {
        case TransferMode::Binary:
            return false;
        case TransferMode::Ascii:
            return true;
        case TransferMode::Automatic:
            return IsAsciiFileName(name);
    }
    return false;
}

namespace {

const size_t kChunkSize = 65536;

std::string TempError(const std::string & path, const char * what)
{
    return std::string(what) + " '" + path + "'";
}

} // namespace

bool ConvertToUnixLineEndings(const std::string & path,
    unsigned long long & bytes, std::string & error)
{
    FILE * file = fopen(path.c_str(), "rb");
    if (file == NULL)
    {
        error = TempError(path, "Cannot open");
        return false;
    }
    std::string temp = path + ".lscp-ascii-tmp";
    FILE * out = fopen(temp.c_str(), "wb");
    if (out == NULL)
    {
        fclose(file);
        error = TempError(temp, "Cannot create the conversion file");
        return false;
    }

    std::vector<char> buffer(kChunkSize);
    bool previousCR = false;
    unsigned long long written = 0;
    bool ok = true;
    while (ok)
    {
        const size_t got = fread(buffer.data(), 1, buffer.size(), file);
        if (got == 0)
        {
            if (ferror(file))
            {
                error = TempError(path, "Cannot read");
                ok = false;
            }
            break;
        }
        std::vector<char> converted;
        converted.reserve(got);
        for (size_t i = 0; i < got; i++)
        {
            const char c = buffer[i];
            if (previousCR)
            {
                /* The CR (with or without an LF behind it) is one newline. */
                previousCR = false;
                converted.push_back('\n');
                if (c == '\n')
                {
                    continue;
                }
            }
            if (c == '\r')
            {
                previousCR = true;
                continue;
            }
            converted.push_back(c);
        }
        if (!converted.empty() &&
            fwrite(converted.data(), 1, converted.size(), out) != converted.size())
        {
            error = TempError(temp, "Cannot write");
            ok = false;
            break;
        }
        written += converted.size();
    }
    if (ok && previousCR)
    {
        /* The file ended with a lone CR. */
        if (fputc('\n', out) == EOF)
        {
            error = TempError(temp, "Cannot write");
            ok = false;
        }
        else
        {
            written += 1;
        }
    }
    if (fclose(file) != 0 && ok)
    {
        error = TempError(path, "Cannot read");
        ok = false;
    }
    if (fclose(out) != 0 && ok)
    {
        error = TempError(temp, "Cannot write");
        ok = false;
    }
    if (!ok)
    {
        remove(temp.c_str());
        return false;
    }
    if (remove(path.c_str()) != 0 || rename(temp.c_str(), path.c_str()) != 0)
    {
        error = TempError(path, "Cannot replace");
        remove(temp.c_str());
        return false;
    }
    bytes = written;
    return true;
}

bool MakeCRLFUploadCopy(const std::string & source, std::string & tempPath,
    std::string & error)
{
    FILE * file = fopen(source.c_str(), "rb");
    if (file == NULL)
    {
        error = TempError(source, "Cannot open");
        return false;
    }
    tempPath = source + ".lscp-crlf-tmp";
    FILE * out = fopen(tempPath.c_str(), "wb");
    if (out == NULL)
    {
        fclose(file);
        error = TempError(tempPath, "Cannot create the conversion file");
        return false;
    }

    std::vector<char> buffer(kChunkSize);
    bool previousLF = false;
    bool ok = true;
    while (ok)
    {
        const size_t got = fread(buffer.data(), 1, buffer.size(), file);
        if (got == 0)
        {
            if (ferror(file))
            {
                error = TempError(source, "Cannot read");
                ok = false;
            }
            break;
        }
        std::vector<char> converted;
        converted.reserve(got + got / 8 + 8);
        for (size_t i = 0; i < got; i++)
        {
            const char c = buffer[i];
            if (previousLF)
            {
                /* The CR or CRLF already emitted one newline. */
                previousLF = false;
                if (c == '\n')
                {
                    continue;
                }
            }
            if (c == '\r')
            {
                previousLF = true;
                converted.push_back('\r');
                converted.push_back('\n');
                continue;
            }
            if (c == '\n')
            {
                converted.push_back('\r');
                converted.push_back('\n');
                continue;
            }
            converted.push_back(c);
        }
        if (!converted.empty() &&
            fwrite(converted.data(), 1, converted.size(), out) != converted.size())
        {
            error = TempError(tempPath, "Cannot write");
            ok = false;
            break;
        }
    }
    if (fclose(file) != 0 && ok)
    {
        error = TempError(source, "Cannot read");
        ok = false;
    }
    if (fclose(out) != 0 && ok)
    {
        error = TempError(tempPath, "Cannot write");
        ok = false;
    }
    if (!ok)
    {
        remove(tempPath.c_str());
        tempPath.clear();
        return false;
    }
    return true;
}

} // namespace linuxscp
