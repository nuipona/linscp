/*
 * Editor: the remote editing workflow of the 'edit' command.
 *
 *   stat the remote file  ->  download it to a private temporary file
 *   ->  run the editor  ->  compare the temporary file with the copy
 *   that was downloaded  ->  ask  ->  upload it back.
 *
 * The remote file is checked again after the editor exits: if somebody
 * else changed it in the meantime the upload asks first instead of
 * overwriting their change, and in a non-interactive run it fails.
 */

#include "Editor.h"

#include <cstdio>
#include <cstring>

#include <unistd.h>

#include "Console.h"
#include "LocalFs.h"
#include "PathUtil.h"
#include "Platform.h"
#include "Texts.h"

namespace linuxscp {

namespace {

std::string EnvironmentVariable(const char * name)
{
    const char * value = getenv(name);
    return value != NULL ? std::string(value) : std::string();
}

/* Replaces everything that is not a file name character, so that a
 * remote path such as '/etc/nginx/nginx.conf' becomes one safe local
 * name. */
std::string SanitizeForFileName(const std::string & text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); i++)
    {
        const char c = text[i];
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        result += safe ? c : '_';
    }
    if (result.empty())
    {
        result = "file";
    }
    if (result.size() > 64)
    {
        result = result.substr(result.size() - 64);
    }
    return result;
}

} // namespace

bool LooksLikeText(const char * data, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (data[i] == '\0')
        {
            return false;
        }
    }
    return true;
}

namespace {

bool ReadWholeFile(const std::string & path, std::string & content)
{
    content.clear();
    FILE * file = fopen(path.c_str(), "rb");
    if (file == NULL)
    {
        return false;
    }
    char buffer[8192];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        content.append(buffer, got);
    }
    const bool ok = ferror(file) == 0;
    fclose(file);
    return ok;
}

bool WriteWholeFile(const std::string & path, const std::string & content)
{
    FILE * file = fopen(path.c_str(), "wb");
    if (file == NULL)
    {
        return false;
    }
    const size_t written = content.empty() ? 0 :
        fwrite(content.data(), 1, content.size(), file);
    const bool ok = written == content.size() && fflush(file) == 0;
    fclose(file);
    return ok;
}

bool HasUtf8Bom(const std::string & bytes)
{
    return bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF;
}

bool UsesPureCrlf(const std::string & bytes)
{
    bool sawCrlf = false;
    for (size_t i = 0; i < bytes.size(); i++)
    {
        if (bytes[i] == '\n')
        {
            if (i == 0 || bytes[i - 1] != '\r')
            {
                return false;
            }
            sawCrlf = true;
        }
    }
    return sawCrlf;
}

std::string ConvertNewlinesToLf(const std::string & bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); i++)
    {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
        {
            continue;
        }
        out += bytes[i];
    }
    return out;
}

std::string ConvertNewlinesToCrlf(const std::string & bytes)
{
    std::string out;
    out.reserve(bytes.size() + 32);
    for (size_t i = 0; i < bytes.size(); i++)
    {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
        {
            out += "\r\n";
            i++;
        }
        else if (bytes[i] == '\n')
        {
            out += "\r\n";
        }
        else
        {
            out += bytes[i];
        }
    }
    return out;
}

} // namespace

std::string NormalizeEditedText(const std::string & originalBytes,
    const std::string & editedBytes, const std::string & eolPolicy)
{
    if (!LooksLikeText(editedBytes.data(), editedBytes.size()))
    {
        return editedBytes;
    }
    const bool hadBom = HasUtf8Bom(originalBytes);
    const bool hasBom = HasUtf8Bom(editedBytes);
    std::string body = hasBom ? editedBytes.substr(3) : editedBytes;
    const std::string origBody = hadBom ? originalBytes.substr(3) : originalBytes;

    if (eolPolicy == "lf")
    {
        body = ConvertNewlinesToLf(body);
    }
    else if (eolPolicy == "crlf")
    {
        body = ConvertNewlinesToCrlf(body);
    }
    else if (UsesPureCrlf(origBody))
    {
        body = ConvertNewlinesToCrlf(body);
    }

    if (hadBom || hasBom)
    {
        return std::string("\xEF\xBB\xBF", 3) + body;
    }
    return body;
}

std::vector<std::string> ResolveEditorCommand(const std::string & configured,
    std::string & error)
{
    error.clear();
    std::string command = configured;
    if (command.empty())
    {
        const char * variables[] = { "LINUXSCP_EDITOR", "VISUAL", "EDITOR" };
        for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]); i++)
        {
            const std::string value = EnvironmentVariable(variables[i]);
            if (!value.empty())
            {
                command = value;
                break;
            }
        }
    }
    if (command.empty())
    {
        static const char * const candidates[] = {
            "editor", "nano", "vim", "vi", "emacs", "gedit", "kate", "code"
        };
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        {
            if (!FindExecutableOnPath(candidates[i]).empty())
            {
                command = candidates[i];
                break;
            }
        }
    }
    if (command.empty())
    {
        error = "No editor is configured. Set the LINUXSCP_EDITOR, VISUAL or "
            "EDITOR variable, use 'option editor <command>', or install one of "
            "the common editors.";
        return std::vector<std::string>();
    }

    std::vector<std::string> words = SplitCommandLine(command);
    if (words.empty())
    {
        error = "The editor command '" + command + "' is empty.";
        return std::vector<std::string>();
    }
    if (words[0].find('/') == std::string::npos)
    {
        const std::string resolved = FindExecutableOnPath(words[0]);
        if (resolved.empty())
        {
            error = "The editor '" + words[0] + "' was not found. Set the "
                "LINUXSCP_EDITOR variable or 'option editor <command>' to the "
                "editor to use.";
            return std::vector<std::string>();
        }
        words[0] = resolved;
    }
    return words;
}

bool PrepareEditorTempFile(const std::string & remotePath, std::string & path,
    std::string & error)
{
    std::string root = TempDirectory();
    if (root.empty())
    {
        root = ConfigDirectory();
    }
    if (root.empty())
    {
        error = "There is no temporary or configuration directory for the "
            "editor file.";
        return false;
    }
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "linuxscp-edit-%ld",
        static_cast<long>(getpid()));
    const std::string directory = root + "/." + suffix;
    std::string createError;
    if (!CreatePrivateDirectory(directory, createError))
    {
        error = createError;
        return false;
    }
    path = JoinLocalPath(directory, SanitizeForFileName(RemoteFileName(remotePath)));
    return true;
}

bool RemoteEditor::Edit(const std::string & remotePath, const EditOptions & options,
    bool confirmUpload, std::string & error)
{
    error.clear();

    LscpAttrs attrs = {};
    std::string remoteError;
    const int exists = mAccess.Stat(remotePath, attrs, remoteError);
    if (exists == LSCP_SFTP_STAT_ERROR)
    {
        error = remoteError.empty() ? std::string("Cannot read '") + remotePath + "'."
            : remoteError;
        return false;
    }
    if (exists == LSCP_SFTP_STAT_MISSING)
    {
        error = "There is no remote file '" + remotePath + "'.";
        return false;
    }
    if ((attrs.flags & LSCP_ATTR_PERMISSIONS) != 0 &&
        (attrs.permissions & 0040000) != 0)
    {
        error = "'" + remotePath + "' is a directory, and only files can be edited.";
        return false;
    }
    if ((attrs.flags & LSCP_ATTR_SIZE) != 0 && !options.force &&
        attrs.size > options.maxSize)
    {
        char buffer[512];
        snprintf(buffer, sizeof(buffer),
            "'%s' is %llu bytes, which is over the %llu byte limit of the "
            "editor. Raise it with 'option editmaxsize <bytes>' or use "
            "'edit -force'.",
            remotePath.c_str(), attrs.size, options.maxSize);
        error = buffer;
        return false;
    }
    const unsigned long remoteMtimeBefore =
        (attrs.flags & LSCP_ATTR_ACMODTIME) != 0 ? attrs.mtime : 0;

    std::string tempPath;
    if (!PrepareEditorTempFile(remotePath, tempPath, error))
    {
        return false;
    }
    RemoveLocalFile(tempPath, remoteError); /* ignore: it may not exist */

    if (mConsole != NULL)
    {
        mConsole->Status("Downloading '" + remotePath + "' for editing...");
    }
    if (mAccess.Download(remotePath, tempPath, true, error) != 0)
    {
        RemoveLocalFile(tempPath, remoteError);
        if (error.empty())
        {
            error = std::string("Cannot download '") + remotePath + "'.";
        }
        return false;
    }
    RestrictFileToUser(tempPath);

    const LocalFileInfo before = StatLocal(tempPath);
    std::string beforeBytes;
    ReadWholeFile(tempPath, beforeBytes);
    if (!options.force && !options.allowBinary)
    {
        const size_t checkLen = beforeBytes.size() < 8192 ?
            beforeBytes.size() : 8192;
        if (checkLen != 0 && !LooksLikeText(beforeBytes.data(), checkLen))
        {
            error = "'" + remotePath + "' does not look like a text file. Use "
                "'edit -force' to edit it anyway.";
            if (!options.keepTempFile)
            {
                RemoveLocalFile(tempPath, remoteError);
            }
            return false;
        }
    }

    std::vector<std::string> command = ResolveEditorCommand(options.editor, error);
    if (command.empty())
    {
        if (!options.keepTempFile)
        {
            RemoveLocalFile(tempPath, remoteError);
        }
        return false;
    }
    command.push_back(tempPath);

    if (mConsole != NULL)
    {
        mConsole->Status("Editing '" + tempPath + "'. Save and close the editor "
            "to upload the file back to '" + remotePath + "'.");
    }
    const int exitCode = RunInteractive(command, error);
    if (exitCode < 0)
    {
        if (!options.keepTempFile)
        {
            RemoveLocalFile(tempPath, remoteError);
        }
        if (error.empty())
        {
            error = std::string("Cannot run the editor '") + command[0] + "'.";
        }
        return false;
    }
    if (exitCode != 0)
    {
        /* A non-zero exit is a failed or aborted editor run: keep the
         * file, the user may have work in it. */
        error = "The editor exited with status " + std::to_string(exitCode) +
            "; '" + remotePath + "' was not changed. The local copy is kept at '" +
            tempPath + "'.";
        return false;
    }

    unsigned long expectedRemoteMtime = remoteMtimeBefore;
    std::string lastUploadedBytes = beforeBytes;

    auto uploadIfChanged = [&](bool & uploadedOut) -> bool
    {
        uploadedOut = false;
        const LocalFileInfo after = StatLocal(tempPath);
        if (!after.exists)
        {
            error = "The temporary file '" + tempPath + "' is gone; '" +
                remotePath + "' was not changed.";
            return false;
        }
        std::string editedBytes;
        if (!ReadWholeFile(tempPath, editedBytes))
        {
            error = "Cannot read the edited file '" + tempPath + "'.";
            return false;
        }
        if (!options.force && !options.allowBinary)
        {
            editedBytes = NormalizeEditedText(beforeBytes, editedBytes,
                options.eol);
        }
        if (editedBytes == lastUploadedBytes &&
            after.mtime == before.mtime && after.size == before.size)
        {
            return true;
        }
        if (editedBytes == lastUploadedBytes)
        {
            return true;
        }
        WriteWholeFile(tempPath, editedBytes);

        /* The remote file may have changed while the editor was open. */
        LscpAttrs nowAttrs = {};
        std::string statError;
        const int stillThere = mAccess.Stat(remotePath, nowAttrs, statError);
        if (stillThere == LSCP_SFTP_STAT_ERROR)
        {
            error = statError.empty() ? std::string("Cannot re-read '") +
                remotePath + "'." : statError;
            return false;
        }
        if (stillThere == LSCP_SFTP_STAT_MISSING)
        {
            error = "'" + remotePath + "' no longer exists; the edited copy is "
                "kept at '" + tempPath + "'.";
            return false;
        }
        const unsigned long remoteMtimeNow =
            (nowAttrs.flags & LSCP_ATTR_ACMODTIME) != 0 ? nowAttrs.mtime : 0;
        if (expectedRemoteMtime != 0 && remoteMtimeNow != expectedRemoteMtime)
        {
            bool overwrite = false;
            if (confirmUpload && mConsole != NULL && mConsole->IsInteractive())
            {
                overwrite = mConsole->Confirm("'" + remotePath +
                    "' changed on the server while it was open in the editor. "
                    "Overwrite it with your changes?", false);
            }
            if (!overwrite)
            {
                error = "'" + remotePath + "' changed on the server while it was "
                    "being edited, so it was not overwritten. Your copy is kept at '" +
                    tempPath + "'.";
                return false;
            }
        }

        if (confirmUpload && mConsole != NULL && mConsole->IsInteractive() &&
            !mConsole->Confirm("Upload the changes to '" + remotePath + "'?", true))
        {
            if (mConsole != NULL)
            {
                mConsole->Status("Cancelled. The edited copy is kept at '" +
                    tempPath + "'.");
            }
            return true;
        }

        if (mConsole != NULL)
        {
            mConsole->Status("Uploading '" + remotePath + "'...");
        }
        if (mAccess.Upload(tempPath, remotePath, false, error) != 0)
        {
            if (error.empty())
            {
                error = std::string("Cannot upload '") + remotePath + "'.";
            }
            error += " The edited copy is kept at '" + tempPath + "'.";
            return false;
        }
        if (mConsole != NULL)
        {
            mConsole->Status("Uploaded '" + remotePath + "'.");
        }
        lastUploadedBytes = editedBytes;
        uploadedOut = true;
        LscpAttrs updatedAttrs = {};
        if (mAccess.Stat(remotePath, updatedAttrs, statError) ==
                LSCP_SFTP_STAT_EXISTS &&
            (updatedAttrs.flags & LSCP_ATTR_ACMODTIME) != 0)
        {
            expectedRemoteMtime = updatedAttrs.mtime;
        }
        return true;
    };

    bool uploaded = false;
    if (!uploadIfChanged(uploaded))
    {
        return false;
    }
    if (!uploaded && !options.monitor)
    {
        if (mConsole != NULL)
        {
            mConsole->Status("No changes to upload; '" + remotePath +
                "' is unchanged.");
        }
        if (!options.keepTempFile)
        {
            RemoveLocalFile(tempPath, remoteError);
        }
        return true;
    }

    if (options.monitor)
    {
        InstallInterruptHandler();
        unsigned completedPasses = 0;
        const unsigned targetPasses = options.monitorPasses == 0 ? 1 :
            options.monitorPasses;
        if (mConsole != NULL)
        {
            mConsole->Status("Monitoring '" + tempPath + "' for local saves...");
        }
        unsigned elapsedPolls = 0;
        while (!Interrupted() && completedPasses < targetPasses)
        {
            SleepMilliseconds(options.monitorIntervalMs > 0 ?
                options.monitorIntervalMs : 50);
            bool passUploaded = false;
            if (!uploadIfChanged(passUploaded))
            {
                return false;
            }
            if (passUploaded)
            {
                completedPasses++;
                elapsedPolls = 0;
            }
            else if (++elapsedPolls > 200)
            {
                break;
            }
        }
    }

    if (!options.keepTempFile)
    {
        RemoveLocalFile(tempPath, remoteError);
    }
    return true;
}

} // namespace linuxscp
