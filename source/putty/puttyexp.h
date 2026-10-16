#ifndef PUTTY_PUTTYEXP_H
#define PUTTY_PUTTYEXP_H

#include "defs.h"

// from ssh.c

int is_ssh(Plug * plug);
int get_ssh_version(Backend * be);
Seat * get_ssh_seat(Plug * plug);
#ifdef FORK_SSH
const ssh_cipher * get_cscipher(Backend * be);
const ssh_cipher * get_sccipher(Backend * be);
#endif
const struct ssh_compressor * get_cscomp(Backend * be);
const struct ssh_decompressor * get_sccomp(Backend * be);
#define FORK_QUERY_REMMAXPKT 1
#define FORK_QUERY_MAIN_CHANNEL 2
#define FORK_QUERY_TIMER 3
unsigned int linscp_query(Backend * be, int query);
void md5checksum(const char * buffer, int len, unsigned char output[16]);
typedef const struct ssh_keyalg * cp_ssh_keyalg;
void get_hostkey_algs(int type, int * count, cp_ssh_keyalg ** sign_keys);
void get_macs(int * count, const struct ssh2_macalg * const ** amacs);
int have_any_ssh2_hostkey(Seat * seat, const char * host, int port);

// from wingss.c

#include "ssh/gss.h"

void wingss_cleanup(void);

// from portfwd.c

int is_pfwd(Plug * plug);
Seat * get_pfwd_seat(Plug * plug);

// from winnet.c

#ifdef _WIN32
/* Windows' front end drives socket results through window messages. */
void select_result(WPARAM wParam, LPARAM lParam);
#endif

// from sshaesold.c

void *aesold_make_context(void);
void aesold_free_context(void *handle);
void call_aesold_setup(void * ctx, int blocklen, unsigned char * key, int keylen);
void call_aesold_encrypt(void * ctx, unsigned int * block);

// from winmisc.c

void win_misc_cleanup(void);

// from misc.c

const char * get_putty_version(void);

// from winsecur.c

void win_secur_cleanup(void);

// from sshecc.c

void ec_cleanup(void);

// from agent-client.c

typedef enum AuthAgentImplementation {
    AAI_PAGEANT,
    AAI_OPENSSH
} AuthAgentImplementation;

extern AuthAgentImplementation auth_agent_implementation;

#endif
