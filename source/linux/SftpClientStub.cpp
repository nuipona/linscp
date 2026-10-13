/*
 * SftpClientStub.c: link-time stand-ins for the PuTTY SFTP client API
 * (SftpClient.h) in builds without the native PuTTY transport.
 *
 * On Windows every protocol, SFTP included, is served by the curl
 * backend (ProtocolClient.cpp), and Script/Session dispatch through
 * UseNativeSftp() so these functions are never actually called. They
 * exist so the front end links unchanged, and they fail cleanly with
 * an explanatory error if a future bug ever routes a call here.
 *
 * This file is used instead of SftpClient.c in the Windows build; the
 * POSIX build always uses the real SftpClient.c.
 */

#include <stddef.h>

#include "SftpClient.h"

static const char g_stub_error[] =
    "The native SFTP transport is not available in this build; "
    "SFTP is served by curl instead.";

const char * lscp_sftp_error(void)
{
    return g_stub_error;
}

void lscp_sftp_cleanup(void)
{
}

int lscp_sftp_init(void)
{
    return -1;
}

char * lscp_sftp_realpath(const char * path)
{
    (void)path;
    return NULL;
}

LscpNames * lscp_sftp_list_dir(const char * path)
{
    (void)path;
    return NULL;
}

void lscp_sftp_free_names(LscpNames * names)
{
    (void)names;
}

int lscp_sftp_stat(const char * path, LscpAttrs * attrs)
{
    (void)path;
    (void)attrs;
    return LSCP_SFTP_STAT_ERROR;
}

int lscp_sftp_mkdir(const char * path, int permissions)
{
    (void)path;
    (void)permissions;
    return -1;
}

int lscp_sftp_rmdir(const char * path)
{
    (void)path;
    return -1;
}

int lscp_sftp_remove(const char * path)
{
    (void)path;
    return -1;
}

int lscp_sftp_rename(const char * from, const char * to)
{
    (void)from;
    (void)to;
    return -1;
}

int lscp_sftp_set_permissions(const char * path, int permissions)
{
    (void)path;
    (void)permissions;
    return -1;
}

int lscp_sftp_get(const char * remotePath, const char * localPath,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    (void)remotePath;
    (void)localPath;
    (void)size;
    (void)progress;
    (void)progressCtx;
    return -1;
}

int lscp_sftp_get_resume(const char * remotePath, const char * localPath,
    unsigned long long resumeOffset, unsigned long long * size,
    LscpProgressFn progress, void * progressCtx)
{
    (void)remotePath;
    (void)localPath;
    (void)resumeOffset;
    (void)size;
    (void)progress;
    (void)progressCtx;
    return -1;
}

int lscp_sftp_put(const char * localPath, const char * remotePath, int permissions,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx)
{
    (void)localPath;
    (void)remotePath;
    (void)permissions;
    (void)size;
    (void)progress;
    (void)progressCtx;
    return -1;
}

int lscp_sftp_put_resume(const char * localPath, const char * remotePath, int permissions,
    unsigned long long resumeOffset, unsigned long long * size,
    LscpProgressFn progress, void * progressCtx)
{
    (void)localPath;
    (void)remotePath;
    (void)permissions;
    (void)resumeOffset;
    (void)size;
    (void)progress;
    (void)progressCtx;
    return -1;
}
