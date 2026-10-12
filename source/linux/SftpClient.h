/*
 * SftpClient: the SFTP protocol operations of the POSIX front end.
 *
 * This module is compiled as C on purpose: it drives libs/putty/ssh/sftp.c,
 * whose header uses `new' as a structure member name (SftpServerVtable::new)
 * and therefore cannot be included from C++. The front end gets a plain C
 * API with none of the PuTTY types in it.
 *
 * The transport (sftp_senddata/sftp_recvdata/sftp_sendbuffer) is provided
 * by PuttyGlue.cpp, which forwards to the open Session.
 */

#ifndef LINUXSCP_SFTPCLIENT_H
#define LINUXSCP_SFTPCLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSCP_ATTR_SIZE        0x00000001u
#define LSCP_ATTR_UIDGID      0x00000002u
#define LSCP_ATTR_PERMISSIONS 0x00000004u
#define LSCP_ATTR_ACMODTIME   0x00000008u

typedef struct LscpAttrs
{
    unsigned long flags;
    unsigned long long size;
    unsigned long uid;
    unsigned long gid;
    unsigned long permissions;
    unsigned long atime;
    unsigned long mtime;
} LscpAttrs;

typedef struct LscpName
{
    char * filename;
    char * longname;
    LscpAttrs attrs;
} LscpName;

typedef struct LscpNames
{
    int count;
    LscpName * names;
} LscpNames;

typedef void (*LscpProgressFn)(unsigned long long transferred,
    unsigned long long total, void * ctx);

/* All functions return 0 on success and -1 on failure; the reason is then
 * available from lscp_sftp_error(). */

int lscp_sftp_init(void);
const char * lscp_sftp_error(void);

/* Returns a newly allocated absolute path, or NULL. */
char * lscp_sftp_realpath(const char * path);

/* Returns a newly allocated listing, or NULL. Free with lscp_sftp_free_names. */
LscpNames * lscp_sftp_list_dir(const char * path);
void lscp_sftp_free_names(LscpNames * names);

int lscp_sftp_stat(const char * path, LscpAttrs * attrs);
int lscp_sftp_mkdir(const char * path, int permissions);
int lscp_sftp_rmdir(const char * path);
int lscp_sftp_remove(const char * path);
int lscp_sftp_rename(const char * from, const char * to);
/* permissions < 0 leaves the permissions unchanged */
int lscp_sftp_set_permissions(const char * path, int permissions);

/* size may be NULL. On success it holds the number of bytes transferred. */
int lscp_sftp_get(const char * remotePath, const char * localPath,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx);
int lscp_sftp_put(const char * localPath, const char * remotePath, int permissions,
    unsigned long long * size, LscpProgressFn progress, void * progressCtx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LINUXSCP_SFTPCLIENT_H */
