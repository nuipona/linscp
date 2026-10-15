"""A throwaway WebDAV endpoint for LiveDavTest.py.

TestSftpServer.py covers SFTP, TestS3Server.py covers signed HTTP;
WebDAV is a third dialect (PROPFIND, MKCOL, MOVE, Depth) and needs its
own server. This one is plain HTTP on 127.0.0.1 with HTTP Basic
authentication (testuser / testpass) so that the client's --anyauth
challenge round-trip is exercised, and it speaks only what a filesystem
client needs:

  PROPFIND Depth 0/1 with a 207 multi-status (resourcetype,
  getcontentlength, getlastmodified - the properties LinuxSCP's parser
  reads), MKCOL, GET/HEAD with ranges, PUT, DELETE (409 on a
  non-empty collection, the conflict rmdir depends on), MOVE, and COPY
  with absolute-URI Destination and Overwrite semantics.

Storage is in memory, one fresh process per test. The XML is written
by this file and parsed independently by the client; neither shares
code, so a parser bug has to survive meeting the other's output.

    LSCP_TEST_PORT=0 python3 TestDavServer.py
prints "READY <port>" once listening.
"""

import calendar
import datetime
import json
import re
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

USER = "testuser"
PASSWORD = "testpass"
PROPFIND_BODY = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<d:multistatus xmlns:d="DAV:">\n'
    "%s"
    "</d:multistatus>\n"
)
XML_DECL = '<?xml version="1.0" encoding="utf-8"?>'


def http_date(epoch):
    moment = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    return moment.strftime("%a, %d %b %Y %H:%M:%S GMT")


def xml_escape(value):
    return (value.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;")
            .replace("'", "&apos;"))


class Store:
    """path -> dict(kind='dir'|'file', mtime=epoch, data=bytes).

    '/' is the root collection and always exists. Paths are
    normalized to a leading slash and no trailing slash (except root).
    """

    def __init__(self):
        self.items = {"/": {"kind": "dir", "mtime": _now()}}
        self.copy_count = 0
        self.reject_next_copy = False

    @staticmethod
    def normalize(path):
        path = urllib.parse.unquote(path.split("?")[0].split("#")[0])
        if not path.startswith("/"):
            path = "/" + path
        while len(path) > 1 and path.endswith("/"):
            path = path[:-1]
        return path

    def parent_of(self, path):
        if path == "/":
            return None
        return path.rsplit("/", 1)[0] or "/"

    def get(self, path):
        return self.items.get(self.normalize(path))

    def add_dir(self, path):
        path = self.normalize(path)
        if path in self.items:
            return "exists"
        parent = self.parent_of(path)
        item = self.items.get(parent)
        if item is None or item["kind"] != "dir":
            return "no-parent"
        self.items[path] = {"kind": "dir", "mtime": _now()}
        return "created"

    def add_file(self, path, data):
        path = self.normalize(path)
        parent = self.parent_of(path)
        item = self.items.get(parent)
        if item is None or item["kind"] != "dir":
            return "no-parent"
        existed = path in self.items
        self.items[path] = {"kind": "file", "mtime": _now(), "data": data}
        return "replaced" if existed else "created"

    def delete(self, path):
        path = self.normalize(path)
        item = self.items.get(path)
        if item is None:
            return "missing"
        if item["kind"] == "dir":
            prefix = path if path != "/" else "/"
            children = [p for p in self.items
                        if p != path and p.startswith(
                            prefix + "/" if prefix != "/" else "/")]
            if children:
                return "conflict"
            del self.items[path]
            return "deleted"
        del self.items[path]
        return "deleted"

    def children_of(self, path):
        """Immediate members, sorted - '/' means top-level only."""
        path = self.normalize(path)
        out = []
        for p in sorted(self.items):
            if p == path:
                continue
            if path == "/":
                if p.count("/") == 1:
                    out.append(p)
            elif p.startswith(path + "/") and "/" not in p[len(path) + 1:]:
                out.append(p)
        return out

    def copy(self, src, dest, overwrite):
        src = self.normalize(src)
        dest = self.normalize(dest)
        if src not in self.items:
            return "missing-source"
        if dest == "/" or dest == src or dest.startswith(src + "/") or \
                src.startswith(dest + "/"):
            return "forbidden"
        parent = self.parent_of(dest)
        if parent not in self.items or self.items[parent]["kind"] != "dir":
            return "no-parent"
        replaced = dest in self.items
        if replaced and not overwrite:
            return "precondition"
        source = self.items[src]
        subtree = {p: value for p, value in self.items.items()
                   if p == src or p.startswith(src + "/")}
        if replaced:
            for path in list(self.items):
                if path == dest or path.startswith(dest + "/"):
                    del self.items[path]
        for path, value in subtree.items():
            target = dest + path[len(src):]
            self.items[target] = dict(value)
        self.copy_count += 1
        return "replaced" if replaced else "created"

    def move(self, src, dest, overwrite):
        src = self.normalize(src)
        dest = self.normalize(dest)
        if src not in self.items:
            return "missing-source"
        if dest == src:
            return "created"
        if dest == "/" or dest.startswith(src + "/"):
            return "forbidden"
        item = self.items[src]
        parent = self.parent_of(dest)
        if parent not in self.items or self.items[parent]["kind"] != "dir":
            return "no-parent"
        replaced = dest in self.items
        if replaced and not overwrite:
            return "precondition"
        # Move the subtree: every path under src/ follows src.
        subtree = {p: v for p, v in self.items.items()
                   if p == src or p.startswith(src + "/")}
        for p in sorted(subtree):
            target = dest + p[len(src):]
            self.items[target] = subtree[p]
            if p != src:
                del self.items[p]
        # The source's own key is gone too - the destination replaced it.
        # (dest cannot be inside src: that returned forbidden above.)
        del self.items[src]
        return "replaced" if replaced else "created"


def _now():
    return calendar.timegm(
        datetime.datetime.now(datetime.timezone.utc).timetuple())


class DavHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def authorized(self):
        header = self.headers.get("Authorization", "")
        if header == "Basic " + _basic_token(USER, PASSWORD):
            return True
        self.send_response(401)
        self.send_header("WWW-Authenticate",
                         'Basic realm="linuxscp-dav-test"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    # -- helpers -----------------------------------------------------------
    def send_body(self, status, body, content_type="text/plain", extra=()):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in extra:
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD" and body and status not in (204, 304):
            self.wfile.write(body)

    def send_error_text(self, status, text):
        self.send_body(status, text)

    def local_path(self):
        return urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)

    # -- verbs -------------------------------------------------------------
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("DAV", "1")
        self.send_header("Allow",
                         "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, "
                         "PROPFIND, MOVE, COPY")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def drain_body(self):
        length = int(self.headers.get("Content-Length") or "0")
        if length:
            self.rfile.read(length)

    def do_PROPFIND(self):
        self.drain_body()  # before any answer: 401 included, or the
        # retry that follows would read the stale body as a new request
        if not self.authorized():
            return
        path = self.local_path()
        item = SERVER_STORE.get(path)
        if item is None:
            self.send_error_text(404, "Not Found.")
            return
        depth = self.headers.get("Depth", "infinity")
        if depth == "infinity":
            depth = "1"  # this store has no deep trees worth recursing
        host = self.headers.get("Host", "127.0.0.1")
        scheme = "http"  # plain HTTP on the loopback test port
        wanted = [self.normalize(path)] if depth == "0" else (
            [self.normalize(path)] + SERVER_STORE.children_of(path))
        responses = []
        for target in wanted:
            entry = SERVER_STORE.get(target)
            if entry is None:
                continue
            href = "%s://%s%s" % (scheme, host,
                                  target if target != "/" else "/")
            if entry["kind"] == "dir" and not href.endswith("/"):
                href += "/"
            if entry["kind"] == "dir":
                type_prop = "<d:resourcetype><d:collection/></d:resourcetype>"
                length_prop = "<d:getcontentlength>0</d:getcontentlength>"
            else:
                data = entry.get("data", b"")
                type_prop = "<d:resourcetype/>"
                length_prop = ("<d:getcontentlength>%d</d:getcontentlength>"
                               % len(data))
            responses.append(
                "<d:response><d:href>%s</d:href>"
                "<d:propstat><d:prop>%s%s"
                "<d:getlastmodified>%s</d:getlastmodified>"
                "</d:prop>"
                "<d:status>HTTP/1.1 200 OK</d:status>"
                "</d:propstat></d:response>"
                % (xml_escape(href), type_prop, length_prop,
                   http_date(entry["mtime"])))
        body = PROPFIND_BODY % "".join(responses)
        self.send_body(207, body, "application/xml; charset=utf-8")

    @staticmethod
    def normalize(path):
        return Store.normalize(path)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == "/__test__/reject-next-copy":
            SERVER_STORE.reject_next_copy = True
            self.send_body(204, b"")
            return
        if parsed.path == "/__test__/state":
            body = json.dumps({"copy_count": SERVER_STORE.copy_count}).encode(
                "utf-8")
            self.send_body(200, body, "application/json")
            return
        if not self.authorized():
            return
        path = self.local_path()
        item = SERVER_STORE.get(path)
        if item is None:
            self.send_error_text(404, "Not Found.")
            return
        if item["kind"] == "dir":
            self.send_error_text(405, "The collection is not gettable.")
            return
        data = item.get("data", b"")
        extra = (("Last-Modified", http_date(item["mtime"])),
                 ("Accept-Ranges", "bytes"))
        range_header = self.headers.get("Range")
        if range_header:
            match = re.match(r"bytes=(\d+)-(\d*)$", range_header.strip())
            if match:
                start = int(match.group(1))
                if start >= len(data):
                    self.send_error_text(416, "Range not satisfiable.")
                    return
                end = int(match.group(2)) if match.group(2) else len(data) - 1
                end = min(end, len(data) - 1)
                body = data[start:end + 1]
                extra = extra + (
                    ("Content-Range",
                     "bytes %d-%d/%d" % (start, end, len(data))),)
                self.send_body(206, body,
                               "application/octet-stream", extra)
                return
        self.send_body(200, data, "application/octet-stream", extra)

    def do_HEAD(self):
        self.do_GET()

    def do_PUT(self):
        length = int(self.headers.get("Content-Length") or "0")
        data = self.rfile.read(length) if length else b""
        if not self.authorized():
            return
        path = self.local_path()
        parent = SERVER_STORE.parent_of(SERVER_STORE.normalize(path))
        item = SERVER_STORE.get(parent)
        if item is None or item["kind"] != "dir":
            self.send_error_text(409, "The parent collection does not exist.")
            return
        existed = SERVER_STORE.get(path) is not None
        SERVER_STORE.add_file(path, data)
        self.send_response(204 if existed else 201)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_DELETE(self):
        if not self.authorized():
            return
        result = SERVER_STORE.delete(self.local_path())
        if result == "deleted":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif result == "missing":
            self.send_error_text(404, "Not Found.")
        elif result == "conflict":
            self.send_error_text(409, "The collection is not empty.")
        else:
            self.send_error_text(403, "Forbidden.")

    def do_MKCOL(self):
        if not self.authorized():
            return
        result = SERVER_STORE.add_dir(self.local_path())
        if result == "created":
            self.send_response(201)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif result == "exists":
            self.send_error_text(405, "Already there.")
        elif result == "no-parent":
            self.send_error_text(409, "The parent collection does not exist.")
        else:
            self.send_error_text(403, "Forbidden.")

    def do_MOVE(self):
        if not self.authorized():
            return
        destination = self.headers.get("Destination", "")
        if not destination:
            self.send_error_text(400, "Destination required.")
            return
        parsed = urllib.parse.urlsplit(destination)
        dest = parsed.path or destination
        overwrite = self.headers.get("Overwrite", "T").upper() != "F"
        result = SERVER_STORE.move(self.local_path(), dest, overwrite)
        if result in ("created", "replaced"):
            self.send_response(201 if result == "created" else 204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif result == "missing-source":
            self.send_error_text(404, "Not Found.")
        elif result == "precondition":
            self.send_error_text(412, "The destination exists.")
        elif result == "no-parent":
            self.send_error_text(409, "The destination's parent is missing.")
        else:
            self.send_error_text(403, "Forbidden.")

    def do_COPY(self):
        if not self.authorized():
            return
        if SERVER_STORE.reject_next_copy:
            SERVER_STORE.reject_next_copy = False
            self.send_error_text(405, "COPY is disabled for this request.")
            return
        destination = self.headers.get("Destination", "")
        if not destination:
            self.send_error_text(400, "Destination required.")
            return
        parsed = urllib.parse.urlsplit(destination)
        if parsed.netloc and parsed.netloc.lower() != self.headers.get(
                "Host", "").lower():
            self.send_error_text(502, "Destination must be on this server.")
            return
        dest = parsed.path or destination
        overwrite = self.headers.get("Overwrite", "T").upper() != "F"
        result = SERVER_STORE.copy(self.local_path(), dest, overwrite)
        if result in ("created", "replaced"):
            self.send_response(201 if result == "created" else 204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif result == "missing-source":
            self.send_error_text(404, "Not Found.")
        elif result == "precondition":
            self.send_error_text(412, "The destination exists.")
        elif result == "no-parent":
            self.send_error_text(409, "The destination's parent is missing.")
        else:
            self.send_error_text(403, "Forbidden.")

    def do_POST(self):
        self.send_error_text(405, "POST is not part of this test server.")


def _basic_token(user, password):
    import base64
    return base64.b64encode(("%s:%s" % (user, password)).encode()).decode()


SERVER_STORE = Store()


def main():
    import os
    port = int(os.environ.get("LSCP_TEST_PORT", "0"))
    httpd = ThreadingHTTPServer(("127.0.0.1", port), DavHandler)
    print("READY %d" % httpd.server_address[1], flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
