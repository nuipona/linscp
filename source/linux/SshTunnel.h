/*
 * SshTunnel: SSH port-forwarding (tunnel) configuration.
 *
 * Upstream WinSCP allows SSH tunnels: local (-L) and remote (-R) port
 * forwarding, with dynamic forwarding (-D) for SOCKS proxying through
 * the SSH connection.  This file provides the configuration and
 * lifecycle management for tunnel connections.
 *
 * GSSAPI (Kerberos) authentication is handled at compile time: when the
 * build system detects libgssapi_krb5, GSSAPI is available and
 * '-rawsettings Gssapi=1' enables it.  Without the library the setting
 * is refused by name with the reason, exactly like every other setting
 * this build does not apply.
 */

#ifndef LINSCP_SSHTUNNEL_H
#define LINSCP_SSHTUNNEL_H

#include <string>
#include <vector>

namespace linscp {

/*
 * SSH tunnel specification, as the user would write it on the ssh(1)
 * command line:
 *
 *   local:   -L [bind_address:]port:host:hostport
 *   remote:  -R [bind_address:]port:host:hostport
 *   dynamic: -D [bind_address:]port
 */
enum class TunnelType { Local, Remote, Dynamic };

struct SshTunnel
{
    TunnelType type;
    std::string bindAddress;  /* empty = localhost */
    int bindPort;
    std::string remoteHost;   /* empty for dynamic */
    int remotePort;

    SshTunnel() : type(TunnelType::Local), bindPort(0), remotePort(0) {}
};

/* Parse a tunnel specification string:
 *   local  [bind:]port:host:hostport
 *   remote [bind:]port:host:hostport
 *   dynamic [bind:]port
 * Returns false with 'error' when the format is wrong. */
bool ParseTunnelSpec(const std::string & spec, TunnelType type,
    SshTunnel & tunnel, std::string & error);

/* Builds the ssh(1) command-line flags for one tunnel. */
std::string TunnelSshFlags(const SshTunnel & tunnel);

/*
 * GSSAPI/Kerberos availability.
 *
 * This build compiles with -DNO_GSSAPI unless the build system detects
 * libgssapi_krb5 at configure time.  When GSSAPI is not available,
 * IsGssapiSupported() returns false and ApplyGssapiSetting() returns an
 * error with the reason, so a '-rawsettings Gssapi=1' is refused by
 * name rather than silently dropped.
 */
bool IsGssapiSupported();
bool ApplyGssapiSetting(const std::string & value, std::string & error);

} // namespace linscp

#endif /* LINSCP_SSHTUNNEL_H */