/*
 * PuttyCompat: includes the C headers of the shared PuTTY core from C++.
 *
 * Both putty.h and ssh.h use `new' as a structure member name (for
 * instance ssh_cipheralg::new and SftpServerVtable::new), which C++
 * treats as a keyword. It is renamed for the duration of the includes.
 * Member names do not take part in the layout of a struct, so the core,
 * which is compiled as C, keeps using the original name and the two
 * agree on the layout.
 *
 * Everything the front end needs from the core is visible after
 * including this header; the SFTP client itself (SftpClient.c) is
 * compiled as C and includes ssh/sftp.h directly.
 */

#ifndef LINUXSCP_PUTTYCOMPAT_H
#define LINUXSCP_PUTTYCOMPAT_H

#include <string>

extern "C" {

#define new linuxscp_c_member_new
#include "putty.h"
#include "ssh.h"
#include "storage.h"
#undef new

/*
 * Host key cache lookup, provided by PuttyGlue.cpp. The FORK_BUILD core
 * always asks the Seat about the host key, so the front end is what
 * decides whether a key is already trusted: 1 when a cached key for this
 * host, port and key type exists and equals 'key', 0 when nothing is
 * cached, and -1 when the cached key differs.
 */
int linuxscp_check_cached_host_key(const char * hostname, int port,
    const char * keytype, const char * key);

} /* extern "C" */

namespace linuxscp {

/*
 * Makes a private key file usable by the SSH core, which reads PuTTY key
 * files only. An OpenSSH (old PEM or new format) or ssh.com key is
 * imported and written out as an unencrypted .ppk below the
 * configuration directory. Returns the file to configure as the key
 * file, or an empty string on failure with 'error' set. An empty input
 * returns an empty string and no error.
 */
std::string PreparePrivateKeyFile(const std::string & keyFile,
    std::string & error);

/* Removes the converted key file, if one was created. */
void DiscardImportedPrivateKeyFile();

} /* namespace linuxscp */

#endif /* LINUXSCP_PUTTYCOMPAT_H */
