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

extern "C" {

#define new linuxscp_c_member_new
#include "putty.h"
#include "ssh.h"
#include "storage.h"
#undef new

} /* extern "C" */

#endif /* LINUXSCP_PUTTYCOMPAT_H */
