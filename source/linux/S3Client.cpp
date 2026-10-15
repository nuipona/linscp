/*
 * Amazon S3 over HTTPS: see S3Client.h for the model (path-style
 * buckets, delimiter directories, marker objects, SigV4 everywhere).
 *
 * One curl process per request, no connection of its own - the same
 * shape as the other curl-backed protocols, except that every request
 * must be signed first and every answer is XML (or a HEAD header
 * block) rather than an 'ls -l' line. The status code comes from
 * --write-out instead of curl's exit code: without -f, HTTP errors
 * reach us with the S3 error document intact, which is what turns a
 * bare 403 into "InvalidAccessKeyId: The AWS Access Key Id you
 * provided does not exist in this account".
 */

#include "S3Client.h"

#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"
#include "ProtocolClient.h"
#include "S3SigV4.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace linuxscp {

namespace {

/* Hash of the empty body: S3 wants this header on every signed
 * request that has no payload; uploads declare UNSIGNED-PAYLOAD,
 * which is valid over TLS (the only transport an s3 site gets). */
const char * const kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char * const kUnsignedPayload = "UNSIGNED-PAYLOAD";

std::string ToLower(const std::string & text)
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

bool StartsWith(const std::string & text, const char * prefix)
{
    const size_t len = strlen(prefix);
    return text.size() >= len && text.compare(0, len, prefix) == 0;
}

bool StartsWith(const std::string & text, const std::string & prefix)
{
    return text.size() >= prefix.size() &&
        text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string & text, const char * suffix)
{
    const size_t len = strlen(suffix);
    return text.size() >= len &&
        text.compare(text.size() - len, len, suffix) == 0;
}

char * DupString(const std::string & text)
{
    char * copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy != NULL)
    {
        memcpy(copy, text.data(), text.size());
        copy[text.size()] = '\0';
    }
    return copy;
}

const std::string & CurlExecutable()
{
    static const std::string executable = [] {
        std::string found = FindExecutableOnPath("curl");
        return found.empty() ? std::string("curl") : found;
    }();
    return executable;
}

/* ---- site and path layout ---------------------------------------------- */

struct S3Site
{
    bool ok;
    std::string error;
    std::string accessKey;
    std::string secretKey;
    std::string region;
    /* What goes into the Host header: host plus port when it is not
     * the default, exactly as curl will send it. */
    std::string authority;

    S3Site() : ok(false) {}
};

std::string RegionFromEnvironment()
{
    const char * region = getenv("AWS_REGION");
    if (region != NULL && region[0] != '\0')
    {
        return region;
    }
    region = getenv("AWS_DEFAULT_REGION");
    if (region != NULL && region[0] != '\0')
    {
        return region;
    }
    return std::string();
}

/* s3.<region>.amazonaws.com carries the region in the host name;
 * anything else falls back to the environment, then us-east-1. */
std::string RegionFromHost(const std::string & host)
{
    const std::string lower = ToLower(host);
    if (lower != "amazonaws.com" && !EndsWith(lower, ".amazonaws.com") &&
        !EndsWith(lower, ".amazonaws.com.cn"))
    {
        return std::string();
    }
    std::istringstream stream(lower);
    std::string label;
    std::vector<std::string> labels;
    while (std::getline(stream, label, '.'))
    {
        labels.push_back(label);
    }
    for (size_t i = 0; i + 1 < labels.size(); i++)
    {
        if (labels[i] != "s3")
        {
            continue;
        }
        const std::string & candidate = labels[i + 1];
        if (candidate == "amazonaws")
        {
            return "us-east-1";
        }
        /* Region labels always carry a dash (us-east-1, eu-west-1,
         * ...); 'dualstack' and friends do not. */
        if (candidate.find('-') != std::string::npos)
        {
            return candidate;
        }
    }
    return std::string();
}

S3Site S3SiteOf(const SessionOptions & options)
{
    S3Site site;
    if (ToLower(options.protocol) != "s3")
    {
        site.error = "Not an S3 session.";
        return site;
    }
    if (options.userName.empty())
    {
        site.error = "An Amazon S3 site needs its access key ID as the user "
            "name: s3://ACCESSKEY@host/bucket/ or -username=ACCESSKEY.";
        return site;
    }
    if (options.password.empty())
    {
        site.error = "An Amazon S3 site needs its secret access key as the "
            "password: -password=SECRET or the LINUXSCP_PASSWORD "
            "environment variable. Every request is signed with AWS "
            "Signature Version 4, which is what authenticates S3.";
        return site;
    }
    site.accessKey = options.userName;
    site.secretKey = options.password;
    site.region = RegionFromEnvironment();
    if (site.region.empty())
    {
        site.region = RegionFromHost(options.hostName);
    }
    if (site.region.empty())
    {
        site.region = "us-east-1";
    }
    site.authority = options.hostName;
    if (options.portNumber > 0 && options.portNumber != 443)
    {
        site.authority += ":" + std::to_string(options.portNumber);
    }
    site.ok = true;
    return site;
}

/* Percent-encode every segment of an absolute path; '/' survives. */
std::string EncodePath(const std::string & path)
{
    std::string out;
    if (path.empty() || path[0] != '/')
    {
        out += '/';
    }
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start,
            slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty())
        {
            out += s3sig::UriEncode(part, false);
        }
        if (slash == std::string::npos)
        {
            break;
        }
        out += '/';
        start = slash + 1;
    }
    return out;
}

/*
 * '/bucket/key...' -> bucket and key. A key keeps a trailing slash:
 * 'dir/' is the marker object, 'dir' would be a file name. No bucket
 * in the path is an error (the caller checks for '/' itself first).
 */
bool SplitBucketKey(const std::string & path, std::string & bucket,
    std::string & key, std::string & error)
{
    if (path.empty() || path[0] != '/' || path.size() < 2)
    {
        error = "There is no bucket in the remote path '" + path +
            "' (expected /bucket/...).";
        return false;
    }
    const size_t slash = path.find('/', 1);
    if (slash == std::string::npos)
    {
        bucket = path.substr(1);
        key.clear();
    }
    else
    {
        bucket = path.substr(1, slash - 1);
        key = path.substr(slash + 1);
    }
    if (bucket.empty())
    {
        error = "There is no bucket in the remote path '" + path +
            "' (expected /bucket/...).";
        return false;
    }
    return true;
}

std::string S3Url(const S3Site & site, const std::string & encodedPath,
    const std::string & query)
{
    std::string url = "https://" + site.authority + encodedPath;
    if (!query.empty())
    {
        url += "?" + query;
    }
    return url;
}

/* ---- responses ---------------------------------------------------------- */

struct S3Response
{
    int curlCode;    /* curl's own exit code; 0 even for HTTP errors */
    int httpCode;    /* from --write-out; 0 when nothing was answered */
    std::string out; /* body (or HEAD headers), without the status tail */
    std::string err; /* curl stderr */

    S3Response() : curlCode(0), httpCode(0) {}
};

/*
 * Splits 'BODY\nNNN' (what -w "\n%{http_code}" appends). Returns the
 * code, 0 when the last line is not one, and hands the body back.
 */
int SplitWriteOut(const std::string & out, std::string & body)
{
    const size_t newline = out.rfind('\n');
    if (newline == std::string::npos)
    {
        body.clear();
        return 0;
    }
    const std::string codeText = out.substr(newline + 1);
    if (codeText.size() != 3 ||
        codeText[0] < '0' || codeText[0] > '9' ||
        codeText[1] < '0' || codeText[1] > '9' ||
        codeText[2] < '0' || codeText[2] > '9')
    {
        body = out;
        return 0;
    }
    body = out.substr(0, newline);
    return atoi(codeText.c_str());
}

std::string XmlUnescape(const std::string & text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] != '&')
        {
            out += text[i];
            continue;
        }
        const size_t semi = text.find(';', i);
        if (semi == std::string::npos || semi - i > 10)
        {
            out += text[i];
            continue;
        }
        const std::string entity = text.substr(i + 1, semi - i - 1);
        if (entity == "amp") { out += '&'; i = semi; }
        else if (entity == "lt") { out += '<'; i = semi; }
        else if (entity == "gt") { out += '>'; i = semi; }
        else if (entity == "quot") { out += '"'; i = semi; }
        else if (entity == "apos") { out += '\''; i = semi; }
        else { out += text[i]; }
    }
    return out;
}

/* First '<tag>content</tag>' anywhere; false when absent. */
bool XmlFind(const std::string & text, const char * tag,
    std::string & content)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t start = text.find(open);
    if (start == std::string::npos)
    {
        return false;
    }
    const size_t body = start + open.size();
    const size_t end = text.find(close, body);
    if (end == std::string::npos)
    {
        return false;
    }
    content = XmlUnescape(text.substr(body, end - body));
    return true;
}

/* All '<tag>...</tag>' inner bodies, in document order. */
std::vector<std::string> XmlAll(const std::string & text, const char * tag)
{
    std::vector<std::string> blocks;
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    size_t at = 0;
    while ((at = text.find(open, at)) != std::string::npos)
    {
        const size_t body = at + open.size();
        const size_t end = text.find(close, body);
        if (end == std::string::npos)
        {
            break;
        }
        blocks.push_back(text.substr(body, end - body));
        at = end + close.size();
    }
    return blocks;
}

std::string ErrorFromXml(const std::string & body, int httpCode)
{
    std::string code;
    std::string message;
    XmlFind(body, "Code", code);
    XmlFind(body, "Message", message);
    std::string text = "S3 error " + std::to_string(httpCode);
    if (!code.empty())
    {
        text += " (" + code + ")";
    }
    if (!message.empty())
    {
        text += ": " + message;
    }
    return text + ".";
}

/* '2015-08-30T12:36:00.000Z' -> epoch seconds (0 when unparseable). */
unsigned long Iso8601Epoch(const std::string & value)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
            &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 60)
    {
        return 0;
    }
    /* Days since 1970-01-01 (Howard Hinnant's days_from_civil). */
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (month + (month > 2 ?
        -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = static_cast<long long>(era) * 146097 +
        static_cast<long long>(doe) - 719468;
    return static_cast<unsigned long>(days * 86400LL +
        hour * 3600LL + minute * 60LL + second);
}

std::string TrailingSlashRemoved(const std::string & path)
{
    std::string out = path;
    while (out.size() > 1 && out[out.size() - 1] == '/')
    {
        out.erase(out.size() - 1);
    }
    return out;
}

/* ---- the request -------------------------------------------------------- */

/*
 * Signs and runs one request. Returns false only when the endpoint
 * could not be reached or answered at the transport level; every HTTP
 * answer, error pages included, comes back in 'response'.
 */
bool InvokeCurl(const S3Site & site, const SessionOptions & options,
    const std::string & method, const std::string & encodedPath,
    const std::string & query,
    const std::vector<std::pair<std::string, std::string> > & signedExtras,
    const std::string & payloadHash,
    const std::vector<std::string> & extraArgs,
    const std::string & outFile, S3Response & response,
    std::string & transportError)
{
    const unsigned long long now = static_cast<unsigned long long>(time(NULL));
    const s3sig::SignedRequest signed1 = s3sig::Sign(method, encodedPath,
        query, site.authority, signedExtras, payloadHash, now, site.region,
        site.accessKey, site.secretKey);

    std::vector<std::string> args;
    args.push_back(CurlExecutable());
    args.push_back("-s");
    args.push_back("-S");
    /*
     * Deliberately no -f: the HTTP status comes from --write-out and
     * the body keeps the S3 error document, which is how a failure
     * gets a Code and a Message instead of "curl (22)".
     */
    if (options.hostKey == "*")
    {
        args.push_back("--insecure");
    }
    if (options.timeoutSecs > 0)
    {
        args.push_back("--connect-timeout");
        args.push_back(std::to_string(options.timeoutSecs));
    }
    args.push_back("-X");
    args.push_back(method);
    for (size_t i = 0; i < signed1.headers.size(); i++)
    {
        args.push_back("-H");
        args.push_back(signed1.headers[i].first + ": " +
            signed1.headers[i].second);
    }
    args.push_back("-H");
    args.push_back("Authorization: " + signed1.authorization);
    for (size_t i = 0; i < extraArgs.size(); i++)
    {
        args.push_back(extraArgs[i]);
    }
    if (!outFile.empty())
    {
        args.push_back("-o");
        args.push_back(outFile);
    }
    args.push_back("-w");
    args.push_back("\n%{http_code}");
    args.push_back(S3Url(site, encodedPath, query));

    std::string out;
    const int rc = RunCapture(args, out, response.err);
    response.curlCode = rc;
    if (rc == -1 && response.err.empty())
    {
        transportError = "Failed to run curl. Install curl (on Windows "
            "10/11 it ships as curl.exe) and make sure it is on PATH.";
        return false;
    }
    if (rc != 0 && IsCurlEndpointUnreachable(rc))
    {
        transportError = response.err;
        if (transportError.empty())
        {
            transportError = "curl error " + std::to_string(rc) + ".";
        }
        return false;
    }
    std::string body;
    response.httpCode = SplitWriteOut(out, body);
    response.out = body;
    return true;
}

/* Reads a file that curl wrote an error document into, then drops it. */
std::string ConsumeErrorFile(const std::string & path, int httpCode)
{
    std::string body;
    FILE * file = fopen(path.c_str(), "rb");
    if (file != NULL)
    {
        char buffer[2048];
        const size_t got = fread(buffer, 1, sizeof(buffer), file);
        fclose(file);
        body.assign(buffer, got);
    }
    remove(path.c_str());
    if (body.empty())
    {
        return "S3 error " + std::to_string(httpCode) + ".";
    }
    return ErrorFromXml(body, httpCode);
}

/* Failure helper: builds the message for an HTTP-level failure. */
std::string HttpFailure(const S3Response & response, const std::string & what)
{
    if (response.httpCode >= 400)
    {
        return ErrorFromXml(response.out, response.httpCode);
    }
    if (!response.err.empty())
    {
        return what + " failed: " + response.err;
    }
    return what + " failed with curl exit code " +
        std::to_string(response.curlCode) + ".";
}

/* ---- listings ----------------------------------------------------------- */

struct S3Entry
{
    std::string name;
    bool isDir;
    unsigned long long size;
    unsigned long mtime; /* 0 = unknown */
};

bool AppendEntry(LscpNames * names, const std::vector<S3Entry> & entries,
    std::string & error)
{
    names->count = static_cast<int>(entries.size());
    if (names->count == 0)
    {
        names->names = NULL;
        return true;
    }
    names->names = static_cast<LscpName *>(
        calloc(names->count, sizeof(LscpName)));
    if (names->names == NULL)
    {
        names->count = 0;
        error = "Out of memory allocating directory entries.";
        return false;
    }
    for (int i = 0; i < names->count; i++)
    {
        names->names[i].filename = DupString(entries[i].name);
        /* Empty longname: Script formats from the attributes, which is
         * also what keeps the columns stable across locales. */
        names->names[i].longname = DupString("");
        names->names[i].attrs.flags = LSCP_ATTR_PERMISSIONS | LSCP_ATTR_SIZE;
        names->names[i].attrs.permissions = entries[i].isDir ?
            0040755u : 0100644u;
        names->names[i].attrs.size = entries[i].size;
        if (entries[i].mtime != 0)
        {
            names->names[i].attrs.flags |= LSCP_ATTR_ACMODTIME;
            names->names[i].attrs.atime = entries[i].mtime;
            names->names[i].attrs.mtime = entries[i].mtime;
        }
    }
    return true;
}

bool ListBuckets(const S3Site & site, const SessionOptions & options,
    LscpNames * names, std::string & error)
{
    S3Response response;
    if (!InvokeCurl(site, options, "GET", "/", std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(), std::string(),
            response, error))
    {
        return false;
    }
    if (response.httpCode >= 400 || response.curlCode != 0 ||
        response.httpCode == 0)
    {
        error = HttpFailure(response, "ListBuckets");
        return false;
    }
    std::vector<S3Entry> entries;
    const std::vector<std::string> buckets =
        XmlAll(response.out, "Bucket");
    for (size_t i = 0; i < buckets.size(); i++)
    {
        std::string name;
        if (!XmlFind(buckets[i], "Name", name) || name.empty())
        {
            continue;
        }
        S3Entry entry;
        entry.name = name;
        entry.isDir = true;
        entry.size = 0;
        std::string created;
        entry.mtime = XmlFind(buckets[i], "CreationDate", created) ?
            Iso8601Epoch(created) : 0;
        entries.push_back(entry);
    }
    return AppendEntry(names, entries, error);
}

bool ListObjects(const S3Site & site, const SessionOptions & options,
    const std::string & bucket, const std::string & prefixRaw,
    LscpNames * names, std::string & error)
{
    const std::string prefix = prefixRaw;
    std::string query = "list-type=2&delimiter=%2F&prefix=" +
        s3sig::UriEncode(prefix, false);
    std::vector<S3Entry> entries;
    std::map<std::string, size_t> seen;
    std::string continuation;
    const std::string bucketPath = "/" + s3sig::UriEncode(bucket, false);

    for (int page = 0; page < 1000; page++)
    {
        std::string pageQuery = query;
        if (!continuation.empty())
        {
            pageQuery += "&continuation-token=" +
                s3sig::UriEncode(continuation, false);
        }
        S3Response response;
        if (!InvokeCurl(site, options, "GET", bucketPath, pageQuery,
                std::vector<std::pair<std::string, std::string> >(),
                kEmptySha256, std::vector<std::string>(), std::string(),
                response, error))
        {
            return false;
        }
        if (response.httpCode >= 400 || response.curlCode != 0 ||
            response.httpCode == 0)
        {
            error = HttpFailure(response, "ListObjects");
            return false;
        }

        const std::vector<std::string> contents =
            XmlAll(response.out, "Contents");
        for (size_t i = 0; i < contents.size(); i++)
        {
            std::string key;
            if (!XmlFind(contents[i], "Key", key) || key.empty())
            {
                continue;
            }
            if (StartsWith(key, prefix))
            {
                const std::string relative = key.substr(prefix.size());
                if (relative.empty() || relative == ".")
                {
                    continue; /* the prefix itself or its marker */
                }
                if (relative.find('/') != std::string::npos ||
                    EndsWith(relative, "/"))
                {
                    /* Defensive: a server that ignored the delimiter. */
                    continue;
                }
                if (seen.count(relative) != 0)
                {
                    continue;
                }
                S3Entry entry;
                entry.name = relative;
                entry.isDir = false;
                entry.size = 0;
                std::string sizeText;
                if (XmlFind(contents[i], "Size", sizeText))
                {
                    entry.size = strtoull(sizeText.c_str(), NULL, 10);
                }
                std::string modified;
                entry.mtime = XmlFind(contents[i], "LastModified", modified) ?
                    Iso8601Epoch(modified) : 0;
                seen[relative] = entries.size();
                entries.push_back(entry);
            }
        }

        const std::vector<std::string> prefixes =
            XmlAll(response.out, "CommonPrefixes");
        for (size_t i = 0; i < prefixes.size(); i++)
        {
            std::string full;
            if (!XmlFind(prefixes[i], "Prefix", full) || full.empty() ||
                !StartsWith(full, prefix))
            {
                continue;
            }
            std::string relative = full.substr(prefix.size());
            while (!relative.empty() && relative[relative.size() - 1] == '/')
            {
                relative.erase(relative.size() - 1);
            }
            if (relative.empty() || relative.find('/') != std::string::npos)
            {
                continue;
            }
            std::map<std::string, size_t>::iterator at =
                seen.find(relative);
            if (at != seen.end())
            {
                /* A marker object and a prefix with the same name are
                 * one directory to the user; the directory wins. */
                entries[at->second].isDir = true;
                entries[at->second].size = 0;
                continue;
            }
            S3Entry entry;
            entry.name = relative;
            entry.isDir = true;
            entry.size = 0;
            entry.mtime = 0;
            seen[relative] = entries.size();
            entries.push_back(entry);
        }

        std::string truncated;
        if (!XmlFind(response.out, "IsTruncated", truncated) ||
            truncated != "true")
        {
            break;
        }
        if (!XmlFind(response.out, "NextContinuationToken", continuation) ||
            continuation.empty())
        {
            break;
        }
    }

    return AppendEntry(names, entries, error);
}

/* ---- shared request blocks --------------------------------------------- */

/* Runs a signed request and translates the outcome into one of the
 * stat codes, filling 'attrs' when the target exists. */
int StatRequest(const S3Site & site, const SessionOptions & options,
    const std::string & path, LscpAttrs * attrs, std::string & error)
{
    S3Response response;
    if (!InvokeCurl(site, options, "HEAD", EncodePath(path), std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(1, "-I"),
            std::string(), response, error))
    {
        return LSCP_SFTP_STAT_ERROR;
    }
    if (response.curlCode != 0)
    {
        error = HttpFailure(response, "Stat");
        return LSCP_SFTP_STAT_ERROR;
    }
    if (response.httpCode == 0)
    {
        error = "The S3 endpoint returned no status for '" + path + "'.";
        return LSCP_SFTP_STAT_ERROR;
    }
    if (response.httpCode == 404)
    {
        return LSCP_SFTP_STAT_MISSING;
    }
    if (response.httpCode >= 400)
    {
        error = ErrorFromXml(response.out, response.httpCode);
        return LSCP_SFTP_STAT_ERROR;
    }
    if (attrs != NULL)
    {
        attrs->flags = LSCP_ATTR_PERMISSIONS | LSCP_ATTR_SIZE;
        attrs->permissions = EndsWith(path, "/") ? 0040755u : 0100644u;
        attrs->size = 0;
        attrs->atime = 0;
        attrs->mtime = 0;
        /* The HEAD header block, lowercased for the search. */
        const std::string headers = ToLower(response.out);
        const size_t length = headers.find("content-length:");
        if (length != std::string::npos)
        {
            attrs->size = strtoull(headers.c_str() + length +
                strlen("content-length:"), NULL, 10);
        }
        const size_t modified = headers.find("last-modified:");
        if (modified != std::string::npos)
        {
            const size_t value = headers.find_first_not_of(" \t\r",
                modified + strlen("last-modified:"));
            if (value != std::string::npos)
            {
                const unsigned long stamp =
                    ParseHttpDate(response.out.substr(value));
                if (stamp != 0)
                {
                    attrs->flags |= LSCP_ATTR_ACMODTIME;
                    attrs->atime = stamp;
                    attrs->mtime = stamp;
                }
            }
        }
    }
    return LSCP_SFTP_STAT_EXISTS;
}

/* True when there is something under 'prefix' besides the marker of
 * 'prefix' itself. Any list failure is reported as an error. */
bool PrefixHasChildren(const S3Site & site, const SessionOptions & options,
    const std::string & bucket, const std::string & prefix,
    bool & hasChildren, std::string & error)
{
    hasChildren = false;
    const std::string query = "list-type=2&delimiter=%2F&max-keys=11" +
        std::string("&prefix=") + s3sig::UriEncode(prefix, false);
    S3Response response;
    if (!InvokeCurl(site, options, "GET",
            "/" + s3sig::UriEncode(bucket, false), query,
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(), std::string(),
            response, error))
    {
        return false;
    }
    if (response.httpCode >= 400 || response.curlCode != 0 ||
        response.httpCode == 0)
    {
        error = HttpFailure(response, "ListObjects");
        return false;
    }
    const std::vector<std::string> contents =
        XmlAll(response.out, "Contents");
    for (size_t i = 0; i < contents.size(); i++)
    {
        std::string key;
        if (XmlFind(contents[i], "Key", key) && !key.empty() && key != prefix)
        {
            hasChildren = true;
            return true;
        }
    }
    if (!XmlAll(response.out, "CommonPrefixes").empty())
    {
        hasChildren = true;
        return true;
    }
    std::string truncated;
    if (XmlFind(response.out, "IsTruncated", truncated) &&
        truncated == "true")
    {
        hasChildren = true;
    }
    return true;
}

} // namespace

/* ---- public entry points ------------------------------------------------ */

bool S3Client::List(const SessionOptions & options,
    const std::string & remotePath, LscpNames * names, std::string & error)
{
    if (names == NULL)
    {
        error = "Null output pointer.";
        return false;
    }
    names->count = 0;
    names->names = NULL;

    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return false;
    }

    /* Directories are prefixes: the listing path always ends in '/'
     * (the same normalization the curl protocols do), so '/bucket'
     * lists the bucket and '/bucket/docs' only things under 'docs/'. */
    std::string path = remotePath;
    if (path.empty() || path[0] != '/')
    {
        path = "/" + path;
    }
    if (path.size() > 1 && path[path.size() - 1] != '/')
    {
        path += "/";
    }
    if (path == "/")
    {
        return ListBuckets(site, options, names, error);
    }
    std::string bucket;
    std::string key;
    if (!SplitBucketKey(path, bucket, key, error))
    {
        return false;
    }
    return ListObjects(site, options, bucket, key, names, error);
}

int S3Client::Stat(const SessionOptions & options,
    const std::string & remotePath, LscpAttrs * attrs, std::string & error)
{
    if (attrs != NULL)
    {
        memset(attrs, 0, sizeof(*attrs));
    }
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return LSCP_SFTP_STAT_ERROR;
    }
    std::string path = remotePath;
    if (path.empty() || path[0] != '/')
    {
        path = "/" + path;
    }
    if (path == "/")
    {
        if (attrs != NULL)
        {
            attrs->flags = LSCP_ATTR_PERMISSIONS;
            attrs->permissions = 0040755;
        }
        return LSCP_SFTP_STAT_EXISTS;
    }

    std::string bucket;
    std::string key;
    if (!SplitBucketKey(path, bucket, key, error))
    {
        return LSCP_SFTP_STAT_ERROR;
    }

    const int direct = StatRequest(site, options, path, attrs, error);
    if (direct == LSCP_SFTP_STAT_EXISTS && key.empty() && attrs != NULL)
    {
        /* The bucket root is a directory, whatever its HEAD says. */
        attrs->permissions = 0040755;
    }
    if (direct != LSCP_SFTP_STAT_MISSING || key.empty() ||
        EndsWith(key, "/"))
    {
        /* The bucket root and marker paths have no second form. */
        return direct;
    }
    /* 'dir' as a file name may still be a directory: try its marker. */
    error.clear();
    const int marker = StatRequest(site, options, path + "/", attrs, error);
    if (marker == LSCP_SFTP_STAT_EXISTS)
    {
        return LSCP_SFTP_STAT_EXISTS;
    }
    if (marker == LSCP_SFTP_STAT_ERROR)
    {
        return LSCP_SFTP_STAT_ERROR;
    }
    error.clear();
    return LSCP_SFTP_STAT_MISSING;
}

bool S3Client::CheckEndpoint(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    (void)remotePath;
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return false;
    }
    S3Response response;
    if (!InvokeCurl(site, options, "GET", "/", std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(), std::string(),
            response, error))
    {
        return false;
    }
    if (response.httpCode == 0 && response.curlCode != 0)
    {
        error = HttpFailure(response, "Opening the S3 endpoint");
        return false;
    }
    if (response.httpCode == 403)
    {
        std::string code;
        XmlFind(response.out, "Code", code);
        if (code == "SignatureDoesNotMatch" ||
            code == "InvalidAccessKeyId" ||
            code == "ExpiredToken" || code == "InvalidToken")
        {
            /* Credentials the service refuses: there is no session. */
            error = ErrorFromXml(response.out, response.httpCode);
            return false;
        }
        /* Otherwise the credentials are good enough and ListBuckets
         * is merely not permitted - a bucket-scoped policy, which is
         * normal. The server answered; that is the connection. */
    }
    if (response.httpCode >= 500)
    {
        error = ErrorFromXml(response.out, response.httpCode);
        return false;
    }
    return true;
}

int S3Client::Get(const SessionOptions & options,
    const std::string & remotePath, const std::string & localPath,
    unsigned long long resumeOffset, bool preserveTime,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx,
    std::string & error, unsigned speedKBytesPerSecond)
{
    (void)progress;
    (void)progressCtx;
    if (size != NULL)
    {
        *size = 0;
    }
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return -1;
    }
    if (remotePath.empty() || remotePath[0] != '/' ||
        (remotePath.size() > 1 && remotePath[remotePath.size() - 1] == '/'))
    {
        error = "'" + remotePath + "' is not an object name (S3 downloads "
            "objects, not prefixes).";
        return -1;
    }

    std::vector<std::string> extraArgs;
    if (resumeOffset > 0)
    {
        /* curl turns this into a Range header; S3 answers 206 and curl
         * stitches. The Range itself is not signed, which SigV4
         * allows: only what we send as canonical headers must be. */
        extraArgs.push_back("-C");
        extraArgs.push_back(std::to_string(resumeOffset));
    }
    if (preserveTime)
    {
        /* Last-Modified of the object onto the local file. */
        extraArgs.push_back("-R");
    }
    if (speedKBytesPerSecond > 0)
    {
        extraArgs.push_back("--limit-rate");
        extraArgs.push_back(std::to_string(speedKBytesPerSecond) + "k");
    }

    S3Response response;
    if (!InvokeCurl(site, options, "GET", EncodePath(remotePath),
            std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, extraArgs, localPath, response, error))
    {
        return -1;
    }
    if (response.httpCode >= 400)
    {
        error = ConsumeErrorFile(localPath, response.httpCode);
        return -1;
    }
    if (response.curlCode != 0 || response.httpCode == 0)
    {
        error = HttpFailure(response, "Download");
        return -1;
    }

    const LocalFileInfo info = StatLocal(localPath);
    if (info.exists && size != NULL)
    {
        *size = info.size;
    }
    return 0;
}

int S3Client::Put(const SessionOptions & options,
    const std::string & localPath, const std::string & remotePath,
    int permissions, unsigned long long resumeOffset, bool preserveTime,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx,
    std::string & error, unsigned speedKBytesPerSecond)
{
    (void)permissions; /* S3 has no POSIX modes to set. */
    (void)preserveTime; /* object times are server-side (Last-Modified). */
    (void)progress;
    (void)progressCtx;
    (void)resumeOffset; /* no partial PUT outside multipart: full body */
    if (size != NULL)
    {
        *size = 0;
    }
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return -1;
    }
    if (remotePath.empty() || remotePath[0] != '/')
    {
        error = "The remote path '" + remotePath + "' is not absolute.";
        return -1;
    }

    std::vector<std::string> extraArgs;
    extraArgs.push_back("-T");
    extraArgs.push_back(localPath);
    if (speedKBytesPerSecond > 0)
    {
        extraArgs.push_back("--limit-rate");
        extraArgs.push_back(std::to_string(speedKBytesPerSecond) + "k");
    }

    S3Response response;
    if (!InvokeCurl(site, options, "PUT", EncodePath(remotePath),
            std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kUnsignedPayload, extraArgs, std::string(), response, error))
    {
        return -1;
    }
    if (response.httpCode < 200 || response.httpCode >= 300 ||
        response.curlCode != 0)
    {
        error = HttpFailure(response, "Upload");
        return -1;
    }

    const LocalFileInfo info = StatLocal(localPath);
    if (info.exists && size != NULL)
    {
        *size = info.size;
    }
    return 0;
}

int S3Client::Remove(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return -1;
    }
    S3Response response;
    if (!InvokeCurl(site, options, "DELETE", EncodePath(remotePath),
            std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(), std::string(),
            response, error))
    {
        return -1;
    }
    if (response.httpCode == 404)
    {
        error = "No such file or directory. " +
            ErrorFromXml(response.out, response.httpCode);
        return -1;
    }
    if (response.httpCode < 200 || response.httpCode >= 300 ||
        response.curlCode != 0)
    {
        error = HttpFailure(response, "Remove");
        return -1;
    }
    return 0;
}

int S3Client::RemoveDir(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return -1;
    }
    std::string path = remotePath;
    if (path.empty() || path[0] != '/')
    {
        path = "/" + path;
    }
    path = TrailingSlashRemoved(path);
    std::string bucket;
    std::string key;
    if (!SplitBucketKey(path, bucket, key, error))
    {
        return -1;
    }
    if (key.empty())
    {
        error = "The bucket itself cannot be removed with rmdir; use "
            "'rm -r' on its contents (or the S3 console).";
        return -1;
    }
    const std::string prefix = key + "/";
    bool hasChildren = false;
    if (!PrefixHasChildren(site, options, bucket, prefix, hasChildren,
            error))
    {
        return -1;
    }
    if (hasChildren)
    {
        error = "The directory is not empty.";
        return -1;
    }
    S3Response response;
    if (!InvokeCurl(site, options, "DELETE", EncodePath(prefix),
            std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(), std::string(),
            response, error))
    {
        return -1;
    }
    if (response.httpCode == 404)
    {
        /* A virtual directory: children could not have existed (checked
         * above), so there is nothing to delete and nothing is wrong. */
        return 0;
    }
    if (response.httpCode < 200 || response.httpCode >= 300 ||
        response.curlCode != 0)
    {
        error = HttpFailure(response, "Remove directory");
        return -1;
    }
    return 0;
}

int S3Client::MakeDir(const SessionOptions & options,
    const std::string & remotePath, std::string & error)
{
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return -1;
    }
    std::string path = remotePath;
    if (path.empty() || path[0] != '/')
    {
        path = "/" + path;
    }
    path = TrailingSlashRemoved(path);
    std::string bucket;
    std::string key;
    if (!SplitBucketKey(path, bucket, key, error))
    {
        return -1;
    }
    std::string probeError;
    if (key.empty())
    {
        /* '/name' with nothing under it: creating a bucket, which is a
         * PUT of the bucket itself (S3 creates it when it does not
         * exist yet). */
        const int bucketState = StatRequest(site, options, path, NULL,
            probeError);
        if (bucketState == LSCP_SFTP_STAT_EXISTS)
        {
            error = "Cannot create the directory '" + path +
                "': that name already exists.";
            return -1;
        }
        if (bucketState == LSCP_SFTP_STAT_ERROR)
        {
            error = probeError;
            return -1;
        }
        std::vector<std::string> bucketArgs;
        bucketArgs.push_back("-d");
        bucketArgs.push_back("");
        S3Response bucketPut;
        if (!InvokeCurl(site, options, "PUT", EncodePath(path),
                std::string(),
                std::vector<std::pair<std::string, std::string> >(),
                kEmptySha256, bucketArgs, std::string(), bucketPut, error))
        {
            return -1;
        }
        if (bucketPut.httpCode < 200 || bucketPut.httpCode >= 300 ||
            bucketPut.curlCode != 0)
        {
            error = HttpFailure(bucketPut, "Create bucket");
            return -1;
        }
        return 0;
    }

    /* Refuse to turn an existing file (or directory) into a marker. */
    const int direct = StatRequest(site, options, path, NULL, probeError);
    if (direct == LSCP_SFTP_STAT_EXISTS)
    {
        error = "Cannot create the directory '" + path +
            "': that name already exists.";
        return -1;
    }
    if (direct == LSCP_SFTP_STAT_ERROR)
    {
        error = probeError;
        return -1;
    }
    const int marker = StatRequest(site, options, path + "/", NULL,
        probeError);
    if (marker == LSCP_SFTP_STAT_EXISTS)
    {
        error = "Cannot create the directory '" + path +
            "': that name already exists.";
        return -1;
    }
    if (marker == LSCP_SFTP_STAT_ERROR)
    {
        error = probeError;
        return -1;
    }

    /* The marker: a zero-byte object whose key ends in '/'. */
    std::vector<std::string> extraArgs;
    extraArgs.push_back("-d");
    extraArgs.push_back("");
    S3Response response;
    if (!InvokeCurl(site, options, "PUT", EncodePath(path) + "/",
            std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, extraArgs, std::string(), response, error))
    {
        return -1;
    }
    if (response.httpCode < 200 || response.httpCode >= 300 ||
        response.curlCode != 0)
    {
        error = HttpFailure(response, "Create directory");
        return -1;
    }
    return 0;
}

int S3Client::Move(const SessionOptions & options,
    const std::string & fromPath, const std::string & toPath,
    std::string & error)
{
    const S3Site site = S3SiteOf(options);
    if (!site.ok)
    {
        error = site.error;
        return -1;
    }
    if (fromPath.empty() || fromPath[0] != '/' || toPath.empty() ||
        toPath[0] != '/')
    {
        error = "S3 rename needs absolute paths.";
        return -1;
    }

    /* CopyObject: a PUT to the destination with the source in
     * x-amz-copy-source (which must be signed like every x-amz-*
     * header). The source path is percent-encoded as one value. */
    std::vector<std::pair<std::string, std::string> > signed1;
    signed1.push_back(std::make_pair("x-amz-copy-source",
        s3sig::UriEncode(fromPath, true)));
    S3Response response;
    if (!InvokeCurl(site, options, "PUT", EncodePath(toPath), std::string(),
            signed1, kUnsignedPayload, std::vector<std::string>(),
            std::string(), response, error))
    {
        return -1;
    }
    if (response.httpCode < 200 || response.httpCode >= 300 ||
        response.curlCode != 0)
    {
        error = HttpFailure(response, "Rename (copy phase)");
        return -1;
    }

    S3Response delete1;
    if (!InvokeCurl(site, options, "DELETE", EncodePath(fromPath),
            std::string(),
            std::vector<std::pair<std::string, std::string> >(),
            kEmptySha256, std::vector<std::string>(), std::string(),
            delete1, error))
    {
        error = "The copy to '" + toPath + "' succeeded, but removing the "
            "source failed: " + error;
        return -1;
    }
    if (delete1.httpCode == 404)
    {
        return 0; /* already gone; the destination holds the data */
    }
    if (delete1.httpCode < 200 || delete1.httpCode >= 300 ||
        delete1.curlCode != 0)
    {
        error = "The copy to '" + toPath + "' succeeded, but removing the "
            "source failed: " + HttpFailure(delete1, "Remove");
        return -1;
    }
    return 0;
}

} // namespace linuxscp
