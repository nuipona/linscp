/*
 * SftpClient.c: SFTP protocol operations on top of the vendored
 * libs/putty/ssh/sftp.c. See SftpClient.h for why this is C.
 *
 * The request/response pattern is the one psftp uses: register a request,
 * receive a packet, look the request up in it, and hand the packet to the
 * matching *_recv() function (which takes ownership of the packet).
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "putty.h"
#include "tree234.h"
#include "ssh/sftp.h"
#include "psftp.h"

#include "SftpClient.h"

#define TRANSFER_CHUNK 32768

static char g_error[1024];

static void clear_error(void)
{
    g_error[0] = '\0';
}

static void set_error(const char * text)
{
    snprintf(g_error, sizeof(g_error), "%s", text);
}

static void set_error_path(const char * what, const char * path)
{
    const char * reason = fxp_error();
    snprintf(g_error, sizeof(g_error), "%s '%s': %s", what, path,
        reason != NULL && reason[0] != '\0' ? reason : "unknown SFTP error");
}

static void set_system_error(const char * what, const char * path)
{
    const int errorNumber = errno != 0 ? errno : EIO;
    const char * reason = strerror(errorNumber);
    snprintf(g_error, sizeof(g_error), "%s '%s': %s", what, path,
        reason != NULL ? reason : "unknown error");
}

const char * lscp_sftp_error(void)
{
    return g_error;
}

void lscp_sftp_cleanup(void)
{
    sftp_cleanup_request();
    clear_error();
}

/* Receives the response to 'request'. Returns the packet, and the request
 * it belongs to through '*found'; both must be consumed by a *_recv()
 * function (which frees the packet). */
static struct sftp_packet * receive_response(struct sftp_request * request,
    struct sftp_request ** found)
{
    struct sftp_packet * pktin;
    (void)request; /* the request id is looked up in the packet itself */
    pktin = sftp_recv();
    if (pktin == NULL)
    {
        set_error("The connection has been closed by the server.");
        return NULL;
    }
    struct sftp_request * rreq = sftp_find_request(pktin);
    (void)request;
    if (rreq == NULL)
    {
        sftp_pkt_free(pktin);
        set_error("Received a packet that does not belong to any request.");
        return NULL;
    }
    *found = rreq;
    return pktin;
}

/* Closes a handle. The caller decides whether a close error replaces an
 * earlier operation error or is itself fatal. */
static bool close_handle(struct fxp_handle * handle)
{
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;

    sftp_register(request = fxp_close_send(handle));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return false;
    }
    return fxp_close_recv(pktin, found);
}

static void attrs_from_fxp(const struct fxp_attrs * attrs, LscpAttrs * out)
{
    out->flags = attrs->flags;
    out->size = attrs->size;
    out->uid = attrs->uid;
    out->gid = attrs->gid;
    out->permissions = attrs->permissions;
    out->atime = attrs->atime;
    out->mtime = attrs->mtime;
}

int lscp_sftp_init(void)
{
    clear_error();
    if (!fxp_init())
    {
        const char * reason = fxp_error();
        snprintf(g_error, sizeof(g_error), "SFTP protocol initialisation failed%s%s.",
            reason != NULL && reason[0] != '\0' ? ": " : "",
            reason != NULL ? reason : "");
        return -1;
    }
    return 0;
}

char * lscp_sftp_realpath(const char * path)
{
    clear_error();
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    char * result;

    sftp_register(request = fxp_realpath_send(path));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return NULL;
    }
    result = fxp_realpath_recv(pktin, found);
    if (result == NULL)
    {
        set_error_path("Resolving the path", path);
    }
    return result;
}

int lscp_sftp_stat(const char * path, LscpAttrs * attrs)
{
    clear_error();
    if (attrs != NULL)
    {
        memset(attrs, 0, sizeof(*attrs));
    }
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    struct fxp_attrs fxpAttrs;

    sftp_register(request = fxp_stat_send(path));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return -1;
    }
    if (!fxp_stat_recv(pktin, found, &fxpAttrs))
    {
        const int status = fxp_error_type();
        set_error_path("Getting the attributes of", path);
        return status == SSH_FX_NO_SUCH_FILE ?
            LSCP_SFTP_STAT_MISSING : LSCP_SFTP_STAT_ERROR;
    }
    if (attrs != NULL)
    {
        attrs_from_fxp(&fxpAttrs, attrs);
    }
    return LSCP_SFTP_STAT_EXISTS;
}

LscpNames * lscp_sftp_list_dir(const char * path)
{
    clear_error();
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    struct fxp_handle * dir;
    LscpNames * result;
    int capacity = 0;

    sftp_register(request = fxp_opendir_send(path));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return NULL;
    }
    dir = fxp_opendir_recv(pktin, found);
    if (dir == NULL)
    {
        set_error_path("Opening the directory", path);
        return NULL;
    }

    result = snew(LscpNames);
    result->count = 0;
    result->names = NULL;

    for (;;)
    {
        struct fxp_names * names;
        int i;

        sftp_register(request = fxp_readdir_send(dir));
        pktin = receive_response(request, &found);
        if (pktin == NULL)
        {
            lscp_sftp_free_names(result);
            return NULL;
        }
        names = fxp_readdir_recv(pktin, found);
        if (names == NULL)
        {
            /*
             * The end of the directory is reported as a status packet with
             * SSH_FX_EOF rather than as an empty name list.
             */
            if (fxp_error_type() == SSH_FX_EOF)
            {
                break;
            }
            set_error_path("Reading the directory", path);
            close_handle(dir);
            lscp_sftp_free_names(result);
            return NULL;
        }
        if (names->nnames == 0)
        {
            fxp_free_names(names);
            break;
        }
        if (result->count + names->nnames > capacity)
        {
            capacity = result->count + names->nnames;
            result->names = sresize(result->names, capacity, LscpName);
        }
        for (i = 0; i < names->nnames; i++)
        {
            LscpName * entry = &result->names[result->count++];
            entry->filename = dupstr(names->names[i].filename);
            entry->longname = dupstr(names->names[i].longname);
            attrs_from_fxp(&names->names[i].attrs, &entry->attrs);
        }
        fxp_free_names(names);
    }

    if (!close_handle(dir))
    {
        set_error_path("Closing the directory", path);
        lscp_sftp_free_names(result);
        return NULL;
    }
    return result;
}

void lscp_sftp_free_names(LscpNames * names)
{
    int i;
    if (names == NULL)
    {
        return;
    }
    for (i = 0; i < names->count; i++)
    {
        sfree(names->names[i].filename);
        sfree(names->names[i].longname);
    }
    sfree(names->names);
    sfree(names);
}

/* Finishes a request that was registered by the caller and whose *_recv()
 * only reports success or failure. */
static int simple_request(const char * what, const char * path,
    bool (*recv)(struct sftp_packet *, struct sftp_request *))
{
    struct sftp_request * found;
    struct sftp_packet * pktin = receive_response(NULL, &found);
    if (pktin == NULL)
    {
        return -1;
    }
    if (!recv(pktin, found))
    {
        set_error_path(what, path);
        return -1;
    }
    return 0;
}

int lscp_sftp_mkdir(const char * path, int permissions)
{
    clear_error();
    struct fxp_attrs attrs = no_attrs;
    PUT_PERMISSIONS(attrs, permissions);
    sftp_register(fxp_mkdir_send(path, &attrs));
    return simple_request("Creating the directory", path, fxp_mkdir_recv);
}

int lscp_sftp_rmdir(const char * path)
{
    clear_error();
    sftp_register(fxp_rmdir_send(path));
    return simple_request("Removing the directory", path, fxp_rmdir_recv);
}

int lscp_sftp_remove(const char * path)
{
    clear_error();
    sftp_register(fxp_remove_send(path));
    return simple_request("Removing", path, fxp_remove_recv);
}

int lscp_sftp_rename(const char * from, const char * to)
{
    clear_error();
    sftp_register(fxp_rename_send(from, to));
    return simple_request("Moving", from, fxp_rename_recv);
}

int lscp_sftp_set_permissions(const char * path, int permissions)
{
    clear_error();
    struct fxp_attrs attrs = no_attrs;
    PUT_PERMISSIONS(attrs, permissions);
    sftp_register(fxp_setstat_send(path, attrs));
    return simple_request("Changing the permissions of", path, fxp_setstat_recv);
}

int lscp_sftp_set_times(const char * path, unsigned long atime,
    unsigned long mtime)
{
    clear_error();
    struct fxp_attrs attrs = no_attrs;
    attrs.flags |= SSH_FILEXFER_ATTR_ACMODTIME;
    attrs.atime = atime;
    attrs.mtime = mtime;
    sftp_register(fxp_setstat_send(path, attrs));
    return simple_request("Setting the times of", path, fxp_setstat_recv);
}

int lscp_sftp_symlink(const char * target, const char * linkPath)
{
    clear_error();
    sftp_register(fxp_symlink_send(target, linkPath));
    return simple_request("Creating the symbolic link", linkPath,
        fxp_symlink_recv);
}

int lscp_sftp_copy(const char * from, const char * to)
{
    clear_error();

    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;

    sftp_register(request = fxp_open_send(from, SSH_FXF_READ, NULL));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return -1;
    }
    struct fxp_handle * readHandle = fxp_open_recv(pktin, found);
    if (readHandle == NULL)
    {
        set_error_path("Opening the source of the copy", from);
        return -1;
    }

    sftp_register(request = fxp_open_send(to,
        SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC, NULL));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        close_handle(readHandle);
        return -1;
    }
    struct fxp_handle * writeHandle = fxp_open_recv(pktin, found);
    if (writeHandle == NULL)
    {
        set_error_path("Opening the target of the copy", to);
        close_handle(readHandle);
        return -1;
    }

    sftp_register(fxp_copydata_send(readHandle, 0, 0, writeHandle, 0));
    pktin = receive_response(request, &found);
    const bool readClosed = close_handle(readHandle);
    const bool writeClosed = close_handle(writeHandle);
    if (pktin == NULL)
    {
        return -1;
    }
    const bool copied = fxp_copydata_recv(pktin, found);
    if (!copied)
    {
        if (fxp_error_type() == SSH_FX_OP_UNSUPPORTED)
        {
            set_error("The server does not implement the copy-data extension.");
            return -2;
        }
        set_error_path("Copying", from);
        return -1;
    }
    if (!readClosed || !writeClosed)
    {
        set_error("Closing the copy handles failed.");
        return -1;
    }
    return 0;
}

int lscp_sftp_checksum(const char * algorithm, const char * path,
    char * usedAlgorithm, size_t usedAlgorithmSize,
    char * hex, size_t hexSize)
{
    clear_error();
    if (usedAlgorithm != NULL && usedAlgorithmSize > 0)
    {
        usedAlgorithm[0] = '\0';
    }
    if (hex != NULL && hexSize > 0)
    {
        hex[0] = '\0';
    }

    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;

    sftp_register(request = fxp_checkfile_name_send(path, algorithm, 0, 0, 0));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return -1;
    }
    struct fxp_checksum * checksum = fxp_checkfile_name_recv(pktin, found);
    if (checksum == NULL)
    {
        if (fxp_error_type() == SSH_FX_OP_UNSUPPORTED)
        {
            set_error("The server does not implement the check-file-name "
                "extension.");
            return -2;
        }
        set_error_path("Calculating the checksum of", path);
        return -1;
    }

    if (usedAlgorithm != NULL && usedAlgorithmSize > 0)
    {
        snprintf(usedAlgorithm, usedAlgorithmSize, "%s",
            checksum->algorithm != NULL ? checksum->algorithm : "");
    }
    if (hex != NULL && hexSize > 0)
    {
        static const char digits[] = "0123456789abcdef";
        size_t out = 0;
        size_t i;
        for (i = 0; i < checksum->hash_len && out + 2 < hexSize; i++)
        {
            const unsigned char byte = checksum->hash[i];
            hex[out++] = digits[byte >> 4];
            hex[out++] = digits[byte & 0x0f];
        }
        hex[out] = '\0';
    }
    fxp_free_checksum(checksum);
    return 0;
}


static unsigned long long local_file_size(FILE * file)
{
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
    {
        return 0;
    }
    return (unsigned long long)st.st_size;
}

/*
 * One download. 'remoteOffset' is the position in the remote file to read
 * from and 'localOffset' the position in the local file to write at; they
 * are the same for a resume and differ for an append. 'truncateLocal' says
 * whether an existing local file is overwritten.
 */
static int get_transfer(const char * remotePath, const char * localPath,
    unsigned long long remoteOffset, unsigned long long localOffset,
    bool truncateLocal, unsigned long long * size,
    LscpProgressFn progress, void * progressCtx)
{
    clear_error();
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    struct fxp_handle * handle;
    struct fxp_xfer * xfer;
    FILE * file;
    unsigned long long total = 0;
    unsigned long long transferred = localOffset;
    int result = 0;
    bool canCloseHandle = true;
    LscpAttrs attrs;

    if (size != NULL)
    {
        *size = 0;
    }
    if (lscp_sftp_stat(remotePath, &attrs) == LSCP_SFTP_STAT_EXISTS)
    {
        if (attrs.flags & SSH_FILEXFER_ATTR_SIZE)
        {
            total = attrs.size;
        }
    }
    else
    {
        /* The open request below provides the authoritative failure. */
        clear_error();
    }

    sftp_register(request = fxp_open_send(remotePath, SSH_FXF_READ, NULL));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        return -1;
    }
    handle = fxp_open_recv(pktin, found);
    if (handle == NULL)
    {
        set_error_path("Opening the file", remotePath);
        return -1;
    }

    if (truncateLocal)
    {
        file = fopen(localPath, "wb");
    }
    else
    {
        /* Keep what is already there and write over or after it. */
        file = fopen(localPath, localOffset > 0 ? "r+b" : "ab");
        if (file == NULL)
        {
            file = fopen(localPath, "ab");
        }
        if (file != NULL && localOffset > 0)
        {
            fseeko(file, (off_t)localOffset, SEEK_SET);
        }
    }
    if (file == NULL)
    {
        set_system_error("Creating the local file", localPath);
        close_handle(handle);
        return -1;
    }

    xfer = xfer_download_init(handle, remoteOffset);
    while (!xfer_done(xfer))
    {
        int got;

        pktin = sftp_recv();
        if (pktin == NULL)
        {
            set_error("The connection has been closed by the server.");
            result = -1;
            canCloseHandle = false;
            break;
        }
        got = xfer_download_gotpkt(xfer, pktin);
        if (got == INT_MIN)
        {
            sftp_pkt_free(pktin);
            continue;
        }
        if (got < 0)
        {
            set_error_path("Reading the file", remotePath);
            result = -1;
        }
        for (;;)
        {
            void * buffer = NULL;
            int length = 0;
            if (!xfer_download_data(xfer, &buffer, &length))
            {
                break;
            }
            if (result == 0 && length > 0 &&
                fwrite(buffer, 1, (size_t)length, file) != (size_t)length)
            {
                set_system_error("Writing the local file", localPath);
                result = -1;
                xfer_set_error(xfer);
            }
            sfree(buffer);
            if (result == 0)
            {
                transferred += (unsigned long long)length;
                if (progress != NULL)
                {
                    progress(transferred, total, progressCtx);
                }
            }
        }
        /*
         * Refill the pipeline only while the transfer is healthy. If a
         * local write or remote read fails, the remaining queued replies
         * are drained below before the SFTP handle is closed.
         */
        if (result == 0)
        {
            xfer_download_queue(xfer);
        }
    }
    xfer_cleanup(xfer);
    errno = 0;
    if (fclose(file) != 0 && result == 0)
    {
        set_system_error("Closing the local file", localPath);
        result = -1;
    }
    if (canCloseHandle && !close_handle(handle) && result == 0)
    {
        set_error_path("Closing the file", remotePath);
        result = -1;
    }

    if (result != 0)
    {
        return -1;
    }
    if (size != NULL)
    {
        *size = transferred;
    }
    return 0;
}

int lscp_sftp_get_resume(const char * remotePath, const char * localPath,
    unsigned long long resumeOffset, unsigned long long * size,
    LscpProgressFn progress, void * progressCtx)
{
    return get_transfer(remotePath, localPath, resumeOffset, resumeOffset,
        resumeOffset == 0, size, progress, progressCtx);
}

int lscp_sftp_get(const char * remotePath, const char * localPath,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    return get_transfer(remotePath, localPath, 0, 0, true, size, progress,
        progressCtx);
}

/* Size of a local path, or 0 when it does not exist. */
static unsigned long long path_size(const char * path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
    {
        return 0;
    }
    return (unsigned long long)st.st_size;
}

int lscp_sftp_get_append(const char * remotePath, const char * localPath,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    return get_transfer(remotePath, localPath, 0, path_size(localPath), false,
        size, progress, progressCtx);
}

/*
 * One upload, with the local read position, the remote write position and
 * whether the remote file is truncated kept apart so that an append can
 * start at the end of the remote file while reading the local one from its
 * beginning.
 */
static int put_transfer(const char * localPath, const char * remotePath,
    int permissions, unsigned long long localOffset,
    unsigned long long remoteOffset, bool truncateRemote,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    clear_error();
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    struct fxp_handle * handle;
    struct fxp_xfer * xfer;
    struct fxp_attrs attrs = no_attrs;
    FILE * file;
    char * buffer;
    unsigned long long total;
    unsigned long long transferred = remoteOffset;
    size_t pendingWrites = 0;
    bool eof = false;
    bool canCloseHandle = true;
    int result = 0;

    if (size != NULL)
    {
        *size = 0;
    }

    file = fopen(localPath, "rb");
    if (file == NULL)
    {
        set_system_error("Opening the local file", localPath);
        return -1;
    }
    total = local_file_size(file);
    if (localOffset > 0)
    {
        fseeko(file, (off_t)localOffset, SEEK_SET);
    }

    if (permissions >= 0)
    {
        attrs.flags |= SSH_FILEXFER_ATTR_PERMISSIONS;
        attrs.permissions = (unsigned long)permissions;
    }

    int openFlags = SSH_FXF_WRITE | SSH_FXF_CREAT;
    if (truncateRemote)
    {
        openFlags |= SSH_FXF_TRUNC;
    }

    sftp_register(request = fxp_open_send(remotePath, openFlags, &attrs));
    pktin = receive_response(request, &found);
    if (pktin == NULL)
    {
        fclose(file);
        return -1;
    }
    handle = fxp_open_recv(pktin, found);
    if (handle == NULL)
    {
        set_error_path("Creating the file", remotePath);
        fclose(file);
        return -1;
    }

    /*
     * xfer_upload_init() marks the transfer as already at end of file, so
     * xfer_done() reports whether every write request has been
     * acknowledged; 'eof' tracks whether all of the local file has been
     * queued. Keep going until both are done.
     */
    buffer = snewn(TRANSFER_CHUNK, char);
    xfer = xfer_upload_init(handle, remoteOffset);
    while (!eof || pendingWrites != 0)
    {
        size_t read;
        int got;

        while (!eof && xfer_upload_ready(xfer))
        {
            read = fread(buffer, 1, TRANSFER_CHUNK, file);
            if (read == 0)
            {
                if (ferror(file))
                {
                    set_system_error("Reading the local file", localPath);
                    result = -1;
                }
                eof = true;
                break;
            }
            xfer_upload_data(xfer, buffer, (int)read);
            pendingWrites++;
            transferred += read;
            if (progress != NULL)
            {
                progress(transferred, total, progressCtx);
            }
        }
        if (result != 0 || (eof && pendingWrites == 0))
        {
            break;
        }

        pktin = sftp_recv();
        if (pktin == NULL)
        {
            set_error("The connection has been closed by the server.");
            result = -1;
            canCloseHandle = false;
            break;
        }
        got = xfer_upload_gotpkt(xfer, pktin);
        if (got == INT_MIN)
        {
            sftp_pkt_free(pktin);
            continue;
        }
        if (pendingWrites > 0)
        {
            pendingWrites--;
        }
        if (got < 0)
        {
            set_error_path("Writing the file", remotePath);
            result = -1;
            break;
        }
    }

    /* A local read or server write failure does not cancel SFTP requests
     * already in flight. Drain their replies so a later command cannot
     * mistake them for the response to CLOSE or another operation. */
    while (canCloseHandle && pendingWrites > 0)
    {
        int got;
        pktin = sftp_recv();
        if (pktin == NULL)
        {
            set_error("The connection has been closed by the server.");
            result = -1;
            canCloseHandle = false;
            break;
        }
        got = xfer_upload_gotpkt(xfer, pktin);
        if (got == INT_MIN)
        {
            sftp_pkt_free(pktin);
            continue;
        }
        pendingWrites--;
        if (got < 0)
        {
            if (result == 0)
            {
                set_error_path("Writing the file", remotePath);
            }
            result = -1;
        }
    }
    xfer_cleanup(xfer);
    sfree(buffer);
    errno = 0;
    if (fclose(file) != 0 && result == 0)
    {
        set_system_error("Closing the local file", localPath);
        result = -1;
    }
    if (canCloseHandle && !close_handle(handle) && result == 0)
    {
        set_error_path("Closing the file", remotePath);
        result = -1;
    }

    if (result != 0)
    {
        return -1;
    }
    if (size != NULL)
    {
        *size = transferred;
    }
    return 0;
}

int lscp_sftp_put_resume(const char * localPath, const char * remotePath, int permissions,
    unsigned long long resumeOffset, unsigned long long * size,
    LscpProgressFn progress, void * progressCtx)
{
    return put_transfer(localPath, remotePath, permissions, resumeOffset,
        resumeOffset, resumeOffset == 0, size, progress, progressCtx);
}

int lscp_sftp_put(const char * localPath, const char * remotePath, int permissions,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    return put_transfer(localPath, remotePath, permissions, 0, 0, true, size,
        progress, progressCtx);
}

/*
 * Appends the local file to the remote one. The remote size comes from the
 * server, because an upload must continue where the remote file ends; a
 * missing destination simply starts at zero.
 */
int lscp_sftp_put_append(const char * localPath, const char * remotePath,
    int permissions, unsigned long long * size, LscpProgressFn progress,
    void * progressCtx)
{
    LscpAttrs attrs;
    unsigned long long remoteOffset = 0;
    if (lscp_sftp_stat(remotePath, &attrs) == LSCP_SFTP_STAT_EXISTS &&
        (attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0)
    {
        remoteOffset = attrs.size;
    }
    return put_transfer(localPath, remotePath, permissions, 0, remoteOffset,
        false, size, progress, progressCtx);
}

/*
 * psftpcommon.c, which the build links for its listing helper, calls these
 * two when it prints a directory. The front end does its own listing, so
 * they only need to exist; if they are ever used they report to stderr.
 */
void list_directory_from_sftp_warn_unsorted(void)
{
    fputs("Warning: the server returned an unsorted directory listing\n", stderr);
}

void list_directory_from_sftp_print(struct fxp_name * name)
{
    if (name != NULL && name->longname != NULL)
    {
        fputs(name->longname, stdout);
        fputc('\n', stdout);
    }
}
