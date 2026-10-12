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

static void set_error(const char * text)
{
    snprintf(g_error, sizeof(g_error), "%s", text);
}

static void set_error_path(const char * what, const char * path)
{
    snprintf(g_error, sizeof(g_error), "%s '%s': %s", what, path, fxp_error());
}

static void set_system_error(const char * what, const char * path)
{
    snprintf(g_error, sizeof(g_error), "%s '%s': %s", what, path,
        strerror(errno) != NULL ? strerror(errno) : "unknown error");
}

const char * lscp_sftp_error(void)
{
    return g_error;
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

/* Closes a handle, ignoring errors; used on both success and error paths. */
static void close_handle(struct fxp_handle * handle)
{
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;

    sftp_register(request = fxp_close_send(handle));
    pktin = receive_response(request, &found);
    if (pktin != NULL)
    {
        fxp_close_recv(pktin, found);
    }
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
        set_error_path("Getting the attributes of", path);
        return -1;
    }
    if (attrs != NULL)
    {
        attrs_from_fxp(&fxpAttrs, attrs);
    }
    return 0;
}

LscpNames * lscp_sftp_list_dir(const char * path)
{
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

    close_handle(dir);
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
    struct fxp_attrs attrs = no_attrs;
    PUT_PERMISSIONS(attrs, permissions);
    sftp_register(fxp_mkdir_send(path, &attrs));
    return simple_request("Creating the directory", path, fxp_mkdir_recv);
}

int lscp_sftp_rmdir(const char * path)
{
    sftp_register(fxp_rmdir_send(path));
    return simple_request("Removing the directory", path, fxp_rmdir_recv);
}

int lscp_sftp_remove(const char * path)
{
    sftp_register(fxp_remove_send(path));
    return simple_request("Removing", path, fxp_remove_recv);
}

int lscp_sftp_rename(const char * from, const char * to)
{
    sftp_register(fxp_rename_send(from, to));
    return simple_request("Moving", from, fxp_rename_recv);
}

int lscp_sftp_set_permissions(const char * path, int permissions)
{
    struct fxp_attrs attrs = no_attrs;
    PUT_PERMISSIONS(attrs, permissions);
    sftp_register(fxp_setstat_send(path, attrs));
    return simple_request("Changing the permissions of", path, fxp_setstat_recv);
}

static unsigned long long local_file_size(FILE * file)
{
    long pos = ftell(file);
    long end;
    if (pos < 0)
    {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        return 0;
    }
    end = ftell(file);
    fseek(file, pos, SEEK_SET);
    return end > 0 ? (unsigned long long)end : 0;
}

int lscp_sftp_get(const char * remotePath, const char * localPath,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    struct fxp_handle * handle;
    struct fxp_xfer * xfer;
    FILE * file;
    unsigned long long total = 0;
    unsigned long long transferred = 0;
    int result = 0;
    LscpAttrs attrs;

    if (size != NULL)
    {
        *size = 0;
    }
    if (lscp_sftp_stat(remotePath, &attrs) == 0 && (attrs.flags & SSH_FILEXFER_ATTR_SIZE))
    {
        total = attrs.size;
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

    file = fopen(localPath, "wb");
    if (file == NULL)
    {
        set_system_error("Creating the local file", localPath);
        close_handle(handle);
        return -1;
    }

    xfer = xfer_download_init(handle, 0);
    while (!xfer_done(xfer))
    {
        int got;

        pktin = sftp_recv();
        if (pktin == NULL)
        {
            set_error("The connection has been closed by the server.");
            result = -1;
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
            break;
        }
        for (;;)
        {
            void * buffer = NULL;
            int length = 0;
            if (!xfer_download_data(xfer, &buffer, &length))
            {
                break;
            }
            if (length > 0 && fwrite(buffer, 1, length, file) != (size_t)length)
            {
                set_system_error("Writing the local file", localPath);
                sfree(buffer);
                result = -1;
                break;
            }
            sfree(buffer);
            transferred += length;
            if (progress != NULL)
            {
                progress(transferred, total, progressCtx);
            }
        }
        if (result != 0)
        {
            break;
        }
        /*
         * Refill the pipeline: xfer_download_init() only queues the first
         * window's worth of read requests, and nothing else does.
         */
        xfer_download_queue(xfer);
    }
    xfer_cleanup(xfer);
    fclose(file);
    close_handle(handle);

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

int lscp_sftp_put(const char * localPath, const char * remotePath, int permissions,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    struct sftp_request * request;
    struct sftp_request * found;
    struct sftp_packet * pktin;
    struct fxp_handle * handle;
    struct fxp_xfer * xfer;
    struct fxp_attrs attrs = no_attrs;
    FILE * file;
    char * buffer;
    unsigned long long total;
    unsigned long long transferred = 0;
    bool eof = false;
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

    if (permissions >= 0)
    {
        attrs.flags |= SSH_FILEXFER_ATTR_PERMISSIONS;
        attrs.permissions = (unsigned long)permissions;
    }

    sftp_register(request = fxp_open_send(remotePath,
        SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC, &attrs));
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
    xfer = xfer_upload_init(handle, 0);
    while (!eof || !xfer_done(xfer))
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
            transferred += read;
            if (progress != NULL)
            {
                progress(transferred, total, progressCtx);
            }
        }
        if (result != 0)
        {
            break;
        }

        pktin = sftp_recv();
        if (pktin == NULL)
        {
            set_error("The connection has been closed by the server.");
            result = -1;
            break;
        }
        got = xfer_upload_gotpkt(xfer, pktin);
        if (got == INT_MIN)
        {
            sftp_pkt_free(pktin);
            continue;
        }
        if (got < 0)
        {
            set_error_path("Writing the file", remotePath);
            result = -1;
            break;
        }
    }
    xfer_cleanup(xfer);
    sfree(buffer);
    fclose(file);
    close_handle(handle);

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
