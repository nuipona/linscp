/*
 * liblinscp.h -- C API for the LinSCP automation object model.
 *
 * This header defines the stable C ABI that the .NET assembly, Python
 * bindings and other automation clients link against.  The functions
 * mirror the operations the script interpreter provides: session
 * lifecycle, directory listing, file transfer, synchronization and
 * structured error reporting.
 *
 * All strings are UTF-8.  The caller owns every string returned by
 * lscp_*_result_string() and must free it with lscp_free_string().
 *
 * Build:  gcc -shared -fPIC -o liblinscp.so liblinscp.c -llinscp-frontend
 */

#ifndef LIBLINSCP_H
#define LIBLINSCP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque session handle. */
typedef struct lscp_session lscp_session_t;

/* Error codes returned by every function. */
typedef enum {
    LSCP_OK            =  0,
    LSCP_ERROR         = -1,
    LSCP_NOT_CONNECTED = -2,
    LSCP_AUTH_FAILED   = -3,
    LSCP_HOST_KEY      = -4,
    LSCP_TIMEOUT       = -5,
    LSCP_NOT_FOUND     = -6,
    LSCP_EXISTS        = -7
} lscp_status_t;

/* Listing entry returned by lscp_list(). */
typedef struct {
    const char * filename;
    const char * longname;
    int          is_dir;
    uint64_t     size;
    uint64_t     mtime;
    unsigned long permissions;
} lscp_entry_t;

/* Listing result. */
typedef struct {
    int           count;
    lscp_entry_t *entries;
} lscp_list_result_t;

/* Transfer result. */
typedef struct {
    lscp_status_t status;
    uint64_t      bytes;
    const char   *error;
} lscp_transfer_t;

/* Synchronization result. */
typedef struct {
    lscp_status_t status;
    int           uploaded;
    int           downloaded;
    int           deleted;
    int           skipped;
    const char   *error;
} lscp_sync_t;

/* ---- Session lifecycle ------------------------------------------------ */

/* Create a new session.  Returns NULL on allocation failure. */
lscp_session_t * lscp_create_session(void);

/* Set session options before opening.  All strings are copied. */
lscp_status_t lscp_set_option(lscp_session_t *session,
    const char *name, const char *value);

/* Open the session (connects to the server). */
lscp_status_t lscp_open(lscp_session_t *session,
    const char *url, const char *error, size_t error_len);

/* Close the session without freeing the handle. */
void lscp_close(lscp_session_t *session);

/* Free the session handle and all associated resources. */
void lscp_destroy_session(lscp_session_t *session);

/* ---- Directory operations --------------------------------------------- */

/* List a remote directory.  Caller must free with lscp_free_list(). */
lscp_status_t lscp_list(lscp_session_t *session,
    const char *remote_path, lscp_list_result_t *result);

void lscp_free_list(lscp_list_result_t *result);

/* Stat a single path.  Returns LSCP_NOT_FOUND when absent. */
lscp_status_t lscp_stat(lscp_session_t *session,
    const char *remote_path, lscp_entry_t *entry);

/* Create a directory. */
lscp_status_t lscp_mkdir(lscp_session_t *session,
    const char *remote_path);

/* Remove a file. */
lscp_status_t lscp_rm(lscp_session_t *session,
    const char *remote_path);

/* Remove an empty directory. */
lscp_status_t lscp_rmdir(lscp_session_t *session,
    const char *remote_path);

/* Rename/move. */
lscp_status_t lscp_mv(lscp_session_t *session,
    const char *from_path, const char *to_path);

/* ---- File transfer ---------------------------------------------------- */

/* Download a remote file to a local path. */
lscp_transfer_t lscp_get(lscp_session_t *session,
    const char *remote_path, const char *local_path,
    int preserve_time);

/* Upload a local file to a remote path. */
lscp_transfer_t lscp_put(lscp_session_t *session,
    const char *local_path, const char *remote_path,
    int permissions);

/* ---- Synchronization -------------------------------------------------- */

/* Synchronize local and remote directories. */
lscp_sync_t lscp_synchronize(lscp_session_t *session,
    const char *local_dir, const char *remote_dir,
    const char *direction, const char *options);

/* ---- Remote command execution ----------------------------------------- */

/* Run a command on the server (like 'call'). */
lscp_status_t lscp_call(lscp_session_t *session,
    const char *command, char *output, size_t output_len);

/* ---- Error handling --------------------------------------------------- */

/* The last error message for this session. */
const char * lscp_last_error(lscp_session_t *session);

/* Free a string returned by any lscp_* function. */
void lscp_free_string(char *str);

/* Library version. */
const char * lscp_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBLINSCP_H */