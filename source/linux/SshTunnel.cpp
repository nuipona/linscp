/*
 * SshTunnel: SSH port-forwarding and GSSAPI detection.
 */

#include "SshTunnel.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "Platform.h"

namespace linuxscp {

bool ParseTunnelSpec(const std::string & spec, TunnelType type,
    SshTunnel & tunnel, std::string & error)
{
    tunnel.type = type;
    tunnel.bindAddress.clear();
    tunnel.bindPort = 0;
    tunnel.remoteHost.clear();
    tunnel.remotePort = 0;

    if (spec.empty())
    {
        error = "Empty tunnel specification.";
        return false;
    }

    /* Split on colons.  The formats are:
     *   port:host:hostport    (local/remote)
     *   bind:port:host:hostport (local/remote with bind)
     *   port                  (dynamic)
     *   bind:port             (dynamic with bind) */
    std::vector<std::string> parts;
    std::istringstream stream(spec);
    std::string part;
    while (std::getline(stream, part, ':'))
    {
        parts.push_back(part);
    }

    if (type == TunnelType::Dynamic)
    {
        if (parts.size() == 1)
        {
            tunnel.bindPort = atoi(parts[0].c_str());
        }
        else if (parts.size() == 2)
        {
            tunnel.bindAddress = parts[0];
            tunnel.bindPort = atoi(parts[1].c_str());
        }
        else
        {
            error = "Dynamic tunnel format: [bind_address:]port";
            return false;
        }
    }
    else
    {
        if (parts.size() == 3)
        {
            tunnel.bindPort = atoi(parts[0].c_str());
            tunnel.remoteHost = parts[1];
            tunnel.remotePort = atoi(parts[2].c_str());
        }
        else if (parts.size() == 4)
        {
            tunnel.bindAddress = parts[0];
            tunnel.bindPort = atoi(parts[1].c_str());
            tunnel.remoteHost = parts[2];
            tunnel.remotePort = atoi(parts[3].c_str());
        }
        else
        {
            error = "Tunnel format: [bind_address:]port:host:hostport";
            return false;
        }
    }

    if (tunnel.bindPort <= 0 || tunnel.bindPort > 65535)
    {
        error = "Bind port out of range (1-65535).";
        return false;
    }
    if (type != TunnelType::Dynamic)
    {
        if (tunnel.remoteHost.empty())
        {
            error = "Remote host is required for local/remote tunnels.";
            return false;
        }
        if (tunnel.remotePort <= 0 || tunnel.remotePort > 65535)
        {
            error = "Remote port out of range (1-65535).";
            return false;
        }
    }
    return true;
}

std::string TunnelSshFlags(const SshTunnel & tunnel)
{
    std::string flag;
    switch (tunnel.type)
    {
    case TunnelType::Local:   flag = "-L"; break;
    case TunnelType::Remote:  flag = "-R"; break;
    case TunnelType::Dynamic: flag = "-D"; break;
    }
    std::string spec;
    if (!tunnel.bindAddress.empty())
    {
        spec = tunnel.bindAddress + ":";
    }
    spec += std::to_string(tunnel.bindPort);
    if (tunnel.type != TunnelType::Dynamic)
    {
        spec += ":" + tunnel.remoteHost + ":" + std::to_string(tunnel.remotePort);
    }
    return flag + " " + spec;
}

bool IsGssapiSupported()
{
#ifdef NO_GSSAPI
    return false;
#else
    /* Runtime check: look for the GSSAPI library. */
    const std::string lib = FindExecutableOnPath("libgssapi_krb5.so.2");
    return !lib.empty();
#endif
}

bool ApplyGssapiSetting(const std::string & value, std::string & error)
{
    (void)value;
    if (!IsGssapiSupported())
    {
        error = "GSSAPI/Kerberos is not available in this build. "
            "Install libgssapi-krb5-2 and rebuild with -DNO_GSSAPI removed "
            "to enable it.";
        return false;
    }
    /* When GSSAPI is available, the PuTTY core handles it natively
     * through the gssapi import.  The setting is accepted here so that
     * '-rawsettings Gssapi=1' passes validation. */
    return true;
}

} // namespace linuxscp