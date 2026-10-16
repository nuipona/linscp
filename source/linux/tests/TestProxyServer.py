"""A throwaway proxy endpoint for the live suites.

TestSftpServer.py covers SFTP, TestDavServer.py WebDAV and TestS3Server.py
signed HTTP; a session that connects through a proxy needs one more piece,
because the proxy is what the client talks to first. This one speaks the two
dialects a file transfer client meets in practice:

  SOCKS5 (RFC 1928) with the no-authentication and the
  username/password (RFC 1929) methods, and HTTP CONNECT with Basic
  proxy authentication.

Both forward bytes in either direction once the destination answered, so a
whole SSH session or a whole WebDAV request runs through them unchanged.
Everything the client asked for is echoed on standard output, which is what
makes the suites able to tell a connection that went through the proxy from
one that quietly went around it:

    READY <port>              once listening
    CONNECT <host>:<port>     per accepted request
    AUTH <user>               per successful authentication
    REJECT <reason>           per refused request

    LSCP_TEST_PORT=0 LSCP_TEST_PROXY_KIND=socks5 \\
    LSCP_TEST_PROXY_AUTH=user:pass python3 TestProxyServer.py

Standard library only, on every platform the suites run on.
"""

import base64
import os
import select
import socket
import sys
import threading


def recv_exact(sock, count):
    """Exactly count bytes, or None when the peer stopped first."""
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def pipe(left, right):
    """Copy both ways until one side ends."""
    try:
        while True:
            readable, _, errored = select.select([left, right], [],
                                                 [left, right], 60)
            if errored or not readable:
                break
            for sock in readable:
                data = sock.recv(65536)
                if not data:
                    return
                (right if sock is left else left).sendall(data)
    except OSError:
        pass
    finally:
        for sock in (left, right):
            try:
                sock.close()
            except OSError:
                pass


class ProxyServer(threading.Thread):
    """One proxy, listening on a kernel-picked (or requested) port."""

    def __init__(self, kind="socks5", credentials="", host="127.0.0.1",
                 port=0):
        super().__init__(daemon=True)
        self.kind = kind
        self.credentials = credentials
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind((host, port))
        self.listener.listen(16)
        self.port = self.listener.getsockname()[1]
        self.connections = 0

    def run(self):
        while True:
            try:
                client, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(client,),
                             daemon=True).start()

    def close(self):
        try:
            self.listener.close()
        except OSError:
            pass

    def report(self, line):
        print(line, flush=True)

    # -- one request --------------------------------------------------------
    def handle(self, client):
        try:
            if self.kind == "http":
                self.handle_http(client)
            else:
                self.handle_socks5(client)
        except OSError:
            try:
                client.close()
            except OSError:
                pass

    def handle_socks5(self, client):
        header = recv_exact(client, 2)
        if header is None or header[0] != 5 or header[1] == 0:
            client.close()
            return
        methods = recv_exact(client, header[1])
        if methods is None:
            client.close()
            return
        if self.credentials:
            if 2 not in methods:
                # No acceptable method: the client offered none that can
                # authenticate, which is a refusal, not a fallback.
                client.sendall(b"\x05\xff")
                client.close()
                self.report("REJECT no-authentication-method")
                return
            client.sendall(b"\x05\x02")
            auth = recv_exact(client, 2)
            if auth is None or auth[0] != 1:
                client.close()
                return
            user = recv_exact(client, auth[1])
            length = recv_exact(client, 1)
            if user is None or length is None:
                client.close()
                return
            password = recv_exact(client, length[0])
            supplied = "%s:%s" % (user.decode("utf-8", "replace"),
                                  password.decode("utf-8", "replace"))
            if supplied != self.credentials:
                client.sendall(b"\x01\x01")
                client.close()
                self.report("REJECT wrong-password")
                return
            client.sendall(b"\x01\x00")
            self.report("AUTH %s" % user.decode("utf-8", "replace"))
        else:
            if 0 not in methods:
                client.sendall(b"\x05\xff")
                client.close()
                self.report("REJECT no-authentication-method")
                return
            client.sendall(b"\x05\x00")

        request = recv_exact(client, 4)
        if request is None or request[0] != 5:
            client.close()
            return
        if request[1] != 1:
            # Only CONNECT is a file transfer's business.
            client.sendall(b"\x05\x07\x00\x01" + b"\0" * 6)
            client.close()
            self.report("REJECT unsupported-command")
            return
        kind = request[3]
        if kind == 1:
            address = recv_exact(client, 4)
            if address is None:
                client.close()
                return
            host = socket.inet_ntoa(address)
        elif kind == 3:
            length = recv_exact(client, 1)
            if length is None:
                client.close()
                return
            name = recv_exact(client, length[0])
            if name is None:
                client.close()
                return
            host = name.decode("utf-8", "replace")
        else:
            client.sendall(b"\x05\x08\x00\x01" + b"\0" * 6)
            client.close()
            self.report("REJECT unsupported-address")
            return
        port_bytes = recv_exact(client, 2)
        if port_bytes is None:
            client.close()
            return
        port = int.from_bytes(port_bytes, "big")

        target = connect_to(host, port)
        if target is None:
            client.sendall(b"\x05\x05\x00\x01" + b"\0" * 6)
            client.close()
            self.report("REJECT host-unreachable %s:%d" % (host, port))
            return
        client.sendall(b"\x05\x00\x00\x01" + socket.inet_aton("127.0.0.1") +
                       (0).to_bytes(2, "big"))
        self.connections += 1
        self.report("CONNECT %s:%d" % (host, port))
        pipe(client, target)

    def handle_http(self, client):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = client.recv(4096)
            if not chunk:
                client.close()
                return
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        parts = lines[0].split(" ")
        if len(parts) < 2:
            client.close()
            return
        method, target = parts[0], parts[1]
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()

        if self.credentials:
            expected = "Basic " + base64.b64encode(
                self.credentials.encode()).decode()
            if headers.get("proxy-authorization", "") != expected:
                client.sendall(
                    b"HTTP/1.1 407 Proxy Authentication Required\r\n"
                    b"Proxy-Authenticate: Basic realm=\"linscp-test\"\r\n"
                    b"Connection: close\r\n"
                    b"Content-Length: 4\r\n\r\n407\n")
                client.close()
                self.report("REJECT wrong-password")
                return
            self.report("AUTH %s" % self.credentials.split(":")[0])

        if method.upper() != "CONNECT":
            # An absolute-form GET would be a plain-HTTP forward; the
            # suites only send CONNECT (that is what curl does for a
            # proxied TLS or SSH connection).
            client.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n"
                           b"Content-Length: 0\r\n\r\n")
            client.close()
            self.report("REJECT unsupported-method %s" % method)
            return

        host, _, port = target.partition(":")
        target_socket = connect_to(host, int(port or 80))
        if target_socket is None:
            client.sendall(b"HTTP/1.1 502 Bad Gateway\r\n"
                           b"Content-Length: 0\r\n\r\n")
            client.close()
            self.report("REJECT host-unreachable %s" % target)
            return
        client.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        self.connections += 1
        self.report("CONNECT %s:%s" % (host, port))
        if rest:
            target_socket.sendall(rest)
        pipe(client, target_socket)


def connect_to(host, port):
    try:
        target = socket.create_connection((host, port), timeout=15)
    except OSError:
        return None
    target.settimeout(None)
    return target


def main():
    port = int(os.environ.get("LSCP_TEST_PORT", "0"))
    kind = os.environ.get("LSCP_TEST_PROXY_KIND", "socks5")
    credentials = os.environ.get("LSCP_TEST_PROXY_AUTH", "")
    server = ProxyServer(kind=kind, credentials=credentials, port=port)
    server.start()
    print("READY %d" % server.port, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
