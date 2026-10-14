"""A throwaway Amazon S3 endpoint that verifies AWS Signature Version 4.

TestSftpServer.py covers the SFTP protocol; Amazon S3 is a different
transport (signed HTTP) and needs its own server. This one speaks the
subset LinuxSCP's S3 client uses - ListBuckets, ListObjectsV2 with a
delimiter, PUT/GET/HEAD/DELETE of objects, CopyObject for rename,
zero-byte "key/" markers for directories, byte ranges for resumed
downloads - and it refuses anything whose signature it cannot
re-derive.

The verification here is deliberately written from the specification
(hmac/hashlib over the canonical request the client must have built),
not imported from the same code the client uses: a bug shared between
the two sides would make every test pass while real S3 rejects the
requests. The unit suite pins the client to botocore's output and
LiveS3Test pins this server to a botocore-signed request, so both ends
have to agree with the reference implementation.

Run it directly; it prints "READY <port>" when listening over TLS
(the client maps s3:// to https, and the certificate is the test
fixture in fixtures/):

    LSCP_TEST_ROOT=... LSCP_TEST_PORT=0 python3 TestS3Server.py
"""

import base64
import datetime
import hashlib
import hmac
import os
import pathlib
import re
import ssl
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = pathlib.Path(__file__).resolve().parent
ACCESS_KEY = os.environ.get("LSCP_TEST_ACCESS", "AKIDTEST")
SECRET_KEY = os.environ.get("LSCP_TEST_SECRET", "testsecret42")
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
UNRESERVED = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")


def uri_encode(value, keep_slash=False):
    """The SigV4 percent-encoding: uppercase hex, '~' kept, '/' kept
    only where a path says so."""
    out = []
    for byte in value.encode("utf-8"):
        char = chr(byte)
        if char in UNRESERVED or (keep_slash and char == "/"):
            out.append(char)
        else:
            out.append("%%%02X" % byte)
    return "".join(out)


def canonical_query(raw_query):
    """Decode-and-re-encode every pair (idempotent for a correctly
    encoded client), then sort by name and value."""
    pairs = []
    if raw_query:
        for part in raw_query.split("&"):
            if not part:
                continue
            name, _, value = part.partition("=")
            pairs.append((uri_encode(urllib.parse.unquote(name)),
                          uri_encode(urllib.parse.unquote(value))))
    pairs.sort()
    return "&".join("%s=%s" % pair for pair in pairs)


def canonical_path(raw_path):
    """The S3 canonical URI: percent-encoded once, '/' separators kept.
    A correct client's path is already in that form, so decode then
    re-encode is the identity for it and a mismatch for anyone else."""
    decoded = urllib.parse.unquote(raw_path)
    out = []
    for segment in decoded.split("/"):
        out.append(uri_encode(segment))
    return "/".join(out)


def hmac_sha256(key, data):
    if isinstance(key, str):
        key = key.encode("utf-8")
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hmac.new(key, data, hashlib.sha256).digest()


def signing_key(secret, date_stamp, region, service):
    key = hmac_sha256("AWS4" + secret, date_stamp)
    key = hmac_sha256(key, region)
    key = hmac_sha256(key, service)
    return hmac_sha256(key, "aws4_request")


class AuthorizationError(Exception):
    """Anything that makes a request unverifiable, with the S3 code to
    answer with."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def verify_signature(method, raw_path, raw_query, headers, body):
    """Re-derives what the client signed. Raises AuthorizationError."""
    authorization = headers.get("authorization", "")
    match = re.match(
        r"^AWS4-HMAC-SHA256\s+"
        r"Credential=(?P<access>[^/]+)/(?P<date>\d{8})/"
        r"(?P<region>[^/]+)/(?P<service>[^/]+)/aws4_request,\s*"
        r"SignedHeaders=(?P<signed>[^,]+),\s*"
        r"Signature=(?P<signature>[0-9a-f]{64})$",
        authorization)
    if not match:
        raise AuthorizationError(
            "InvalidRequest",
            "The request is not signed with AWS Signature Version 4 "
            "(Authorization: %s)." % (authorization[:80] or "<absent>"))
    if match.group("access") != ACCESS_KEY:
        raise AuthorizationError(
            "InvalidAccessKeyId",
            "The AWS Access Key Id you provided does not exist in our "
            "records.")
    for header in ("x-amz-date", "x-amz-content-sha256"):
        if header not in headers:
            raise AuthorizationError(
                "MissingSecurityHeader",
                "Your request is missing the required header %s." % header)

    payload_hash = headers["x-amz-content-sha256"]
    if payload_hash != "UNSIGNED-PAYLOAD":
        digest = hashlib.sha256(body).hexdigest()
        if not hmac.compare_digest(payload_hash, digest):
            raise AuthorizationError(
                "XAmzContentSHA256Mismatch",
                "The provided 'x-amz-content-sha256' header does not "
                "match what was computed.")

    signed_names = [name.strip() for name in match.group("signed").split(";")]
    canonical_headers = []
    for name in signed_names:
        if name not in headers:
            raise AuthorizationError(
                "SignatureDoesNotMatch",
                "The signature does not match the signed header '%s'." % name)
        value = " ".join(headers[name].strip().split())
        canonical_headers.append("%s:%s\n" % (name, value))
    canonical = "\n".join((method, canonical_path(raw_path),
                           canonical_query(raw_query))) + "\n" + \
        "".join(canonical_headers) + "\n" + \
        ";".join(signed_names) + "\n" + payload_hash

    scope = "%s/%s/%s/aws4_request" % (
        match.group("date"), match.group("region"), match.group("service"))
    to_sign = "\n".join((
        "AWS4-HMAC-SHA256",
        headers["x-amz-date"],
        scope,
        hashlib.sha256(canonical.encode("utf-8")).hexdigest()))
    key = signing_key(SECRET_KEY, match.group("date"),
                      match.group("region"), match.group("service"))
    expected = hmac_sha256(key, to_sign).hex()
    if not hmac.compare_digest(expected, match.group("signature")):
        raise AuthorizationError(
            "SignatureDoesNotMatch",
            "The request signature we calculated does not match the "
            "signature you provided. Check your key and signing method.")


def xml_escape(value):
    return (value.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;")
            .replace("'", "&apos;"))


def iso8601(epoch):
    moment = datetime.datetime.fromtimestamp(
        epoch, datetime.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%S.000Z")


def http_date(epoch):
    moment = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    return moment.strftime("%a, %d %b %Y %H:%M:%S GMT")


class Store:
    """Bucket -> key -> (bytes, last-modified). In memory only."""

    def __init__(self):
        self.buckets = {}  # name -> creation epoch
        self.objects = {}  # (bucket, key) -> (bytes, mtime)

    def ensure_bucket(self, bucket, epoch):
        if bucket not in self.buckets:
            self.buckets[bucket] = epoch

    def put(self, bucket, key, data, epoch):
        self.ensure_bucket(bucket, epoch)
        self.objects[(bucket, key)] = (data, epoch)

    def get(self, bucket, key):
        return self.objects.get((bucket, key))

    def delete(self, bucket, key):
        if (bucket, key) in self.objects:
            del self.objects[(bucket, key)]
            return True
        return False

    def keys_of(self, bucket):
        return sorted(key for (b, key) in self.objects if b == bucket)


STORE = Store()


class S3Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # -- plumbing ---------------------------------------------------------
    def log_message(self, *args):
        pass

    def headers_dict(self):
        # Lower-cased: the canonical form of a header name is its
        # lower-case spelling, and curl writes "Host" while the
        # signature says "host".
        return {name.lower(): value for name, value in self.headers.items()}

    def read_body(self):
        length = int(self.headers.get("Content-Length") or "0")
        return self.rfile.read(length) if length else b""

    def send_xml(self, status, payload):
        body = payload.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def send_error_xml(self, status, code, message):
        self.send_xml(status,
                      '<?xml version="1.0" encoding="UTF-8"?>\n'
                      "<Error><Code>%s</Code><Message>%s</Message>"
                      "<Resource>%s</Resource><RequestId>test</RequestId>"
                      "</Error>" % (xml_escape(code), xml_escape(message),
                                    xml_escape(self.path)))

    def send_bytes(self, status, data, extra=(), content_length=None):
        self.send_response(status)
        for name, value in extra:
            self.send_header(name, value)
        if status not in (204, 304):
            length = len(data) if content_length is None else content_length
            self.send_header("Content-Length", str(length))
        self.end_headers()
        if data and self.command != "HEAD" and status not in (204, 304):
            self.wfile.write(data)

    # -- dispatch ---------------------------------------------------------
    def split_path(self):
        """'/bucket/key...' -> (bucket, key); '/' -> ('', '')."""
        raw = urllib.parse.urlsplit(self.path)
        path = urllib.parse.unquote(raw.path)
        if path == "/" or path == "":
            return "", "", raw.query
        # Drop exactly the leading separator: splitting a path that
        # still starts with '/' would yield an empty first field and
        # silently file everything under bucket ''.
        if path.startswith("/"):
            path = path[1:]
        parts = path.split("/", 1)
        bucket = parts[0]
        key = parts[1] if len(parts) > 1 else ""
        return bucket, key, raw.query

    def handle_any(self, method, body):
        raw_path = urllib.parse.urlsplit(self.path).path
        raw_query = urllib.parse.urlsplit(self.path).query
        headers = self.headers_dict()
        # The client's canonical host header included the port when the
        # URL did; requests.headers gives Host exactly as sent.
        try:
            verify_signature(method, raw_path, raw_query, headers, body)
        except AuthorizationError as failure:
            self.send_error_xml(403, failure.code, failure.message)
            return

        bucket, key, query = self.split_path()
        try:
            if method == "GET" and bucket == "" and key == "":
                self.list_buckets()
            elif method == "GET" and key == "":
                self.list_objects(bucket, query)
            elif method == "HEAD" and key == "":
                self.head_bucket(bucket)
            elif method == "PUT" and key == "" and not body:
                self.create_bucket(bucket)
            elif method == "GET":
                self.get_object(bucket, key)
            elif method == "HEAD":
                self.head_object(bucket, key)
            elif method == "PUT":
                self.put_object(bucket, key, headers, body)
            elif method == "DELETE":
                self.delete_object(bucket, key)
            else:
                self.send_error_xml(405, "MethodNotAllowed",
                                    "Method %s is not allowed." % method)
        except BrokenPipeError:
            pass

    def do_GET(self):
        self.handle_any("GET", b"")

    def do_HEAD(self):
        self.handle_any("HEAD", b"")

    def do_PUT(self):
        self.handle_any("PUT", self.read_body())

    def do_DELETE(self):
        self.handle_any("DELETE", self.read_body())

    # -- operations -------------------------------------------------------
    def list_buckets(self):
        entries = "".join(
            "<Bucket><Name>%s</Name><CreationDate>%s</CreationDate>"
            "</Bucket>" % (xml_escape(name), iso8601(creation))
            for name, creation in sorted(STORE.buckets.items()))
        self.send_xml(200,
                      '<?xml version="1.0" encoding="UTF-8"?>\n'
                      "<ListAllMyBucketsResult>"
                      "<Owner><ID>test</ID><DisplayName>test</DisplayName>"
                      "</Owner><Buckets>%s</Buckets>"
                      "</ListAllMyBucketsResult>" % entries)

    def list_objects(self, bucket, query):
        if bucket not in STORE.buckets:
            self.send_error_xml(404, "NoSuchBucket",
                                "The specified bucket does not exist.")
            return
        params = urllib.parse.parse_qs(query, keep_blank_values=True)
        prefix = params.get("prefix", [""])[0]
        delimiter = params.get("delimiter", [""])[0]
        max_keys = int(params.get("max-keys", ["1000"])[0])
        token = params.get("continuation-token", [""])[0]
        try:
            offset = int(token) if token else 0
        except ValueError:
            offset = 0

        # Split the matching keys into file entries and sub-prefixes,
        # the way ListObjectsV2 with a delimiter does.
        files = []
        common = set()
        for key in STORE.keys_of(bucket):
            if not key.startswith(prefix):
                continue
            relative = key[len(prefix):]
            if relative == "":
                continue
            if delimiter and delimiter in relative:
                common.add(prefix + relative.split(delimiter, 1)[0] +
                           delimiter)
            else:
                files.append(key)
        entries = sorted(("file", name) for name in files)
        entries += sorted(("dir", name) for name in common)
        page = entries[offset:offset + max_keys]
        truncated = offset + len(page) < len(entries)

        def file_xml(key):
            data, mtime = STORE.objects[(bucket, key)]
            return ("<Contents><Key>%s</Key><LastModified>%s</LastModified>"
                    "<ETag>%s</ETag><Size>%d</Size>"
                    "<StorageClass>STANDARD</StorageClass></Contents>"
                    % (xml_escape(key), iso8601(mtime),
                       hashlib.sha256(data).hexdigest()[:32], len(data)))

        def dir_xml(name):
            return "<CommonPrefixes><Prefix>%s</Prefix></CommonPrefixes>" % (
                xml_escape(name))

        body = "".join(
            file_xml(value) if kind == "file" else dir_xml(value)
            for kind, value in page)
        continuation = ""
        if truncated:
            continuation = ("<NextContinuationToken>%d</NextContinuationToken>"
                            % (offset + len(page)))
        self.send_xml(200,
                      '<?xml version="1.0" encoding="UTF-8"?>\n'
                      "<ListBucketResult><Name>%s</Name><Prefix>%s</Prefix>"
                      "<KeyCount>%d</KeyCount><MaxKeys>%d</MaxKeys>"
                      "<Delimiter>%s</Delimiter><IsTruncated>%s</IsTruncated>"
                      "%s%s</ListBucketResult>"
                      % (xml_escape(bucket), xml_escape(prefix), len(page),
                         max_keys, xml_escape(delimiter),
                         "true" if truncated else "false",
                         continuation, body))

    def head_bucket(self, bucket):
        if bucket not in STORE.buckets:
            self.send_error_xml(404, "NoSuchBucket",
                                "The specified bucket does not exist.")
            return
        self.send_bytes(200, b"")

    def create_bucket(self, bucket):
        STORE.ensure_bucket(bucket, datetime.datetime.now(
            datetime.timezone.utc).timestamp())
        self.send_bytes(200, b"", extra=(("ETag", '"%s"' % bucket),))

    def get_object(self, bucket, key):
        record = STORE.get(bucket, key)
        if record is None:
            self.send_error_xml(404, "NoSuchKey",
                                "The specified key does not exist.")
            return
        data, mtime = record
        range_header = self.headers.get("Range")
        if range_header:
            match = re.match(r"bytes=(\d+)-(\d*)$", range_header.strip())
            if not match:
                self.send_error_xml(416, "InvalidRange",
                                    "Malformed Range header.")
                return
            start = int(match.group(1))
            end = int(match.group(2)) if match.group(2) else len(data) - 1
            if start >= len(data):
                self.send_error_xml(
                    416, "InvalidRange",
                    "The requested range is not satisfiable.")
                return
            end = min(end, len(data) - 1)
            sliced = data[start:end + 1]
            self.send_bytes(206, sliced, extra=(
                ("Content-Range",
                 "bytes %d-%d/%d" % (start, end, len(data))),
                ("Last-Modified", http_date(mtime)),
                ("Accept-Ranges", "bytes"),
                ("Content-Type", "application/octet-stream"),
            ))
            return
        self.send_bytes(200, data, extra=(
            ("Last-Modified", http_date(mtime)),
            ("Accept-Ranges", "bytes"),
            ("Content-Type", "application/octet-stream"),
        ))

    def head_object(self, bucket, key):
        record = STORE.get(bucket, key)
        if record is None:
            self.send_error_xml(404, "NoSuchKey",
                                "The specified key does not exist.")
            return
        data, mtime = record
        self.send_bytes(200, b"", extra=(
            ("Last-Modified", http_date(mtime)),
            ("Content-Type", "application/octet-stream"),
        ), content_length=len(data))

    def put_object(self, bucket, key, headers, body):
        if key == "":
            # PUT /bucket with a body has no meaning here.
            self.send_error_xml(400, "InvalidBucketName",
                                "The bucket name is invalid.")
            return
        copy_source = headers.get("x-amz-copy-source")
        now = datetime.datetime.now(datetime.timezone.utc).timestamp()
        if copy_source:
            decoded = urllib.parse.unquote(copy_source)
            parts = decoded.lstrip("/").split("/", 1)
            source_bucket, source_key = parts[0], parts[1] if len(parts) > 1 else ""
            record = STORE.get(source_bucket, source_key)
            if record is None:
                self.send_error_xml(404, "NoSuchKey",
                                    "The specified key does not exist.")
                return
            STORE.put(bucket, key, record[0], now)
            self.send_xml(200,
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<CopyObjectResult><ETag>%s</ETag>"
                          "<LastModified>%s</LastModified></CopyObjectResult>"
                          % (hashlib.sha256(record[0]).hexdigest()[:32],
                             iso8601(now)))
            return
        if bucket not in STORE.buckets and key.endswith("/"):
            # mkdir creates both the bucket and its marker.
            STORE.ensure_bucket(bucket, now)
        STORE.put(bucket, key, body, now)
        self.send_bytes(200, b"", extra=(
            ("ETag", '"%s"' % hashlib.sha256(body).hexdigest()[:32]),))

    def delete_object(self, bucket, key):
        if bucket not in STORE.buckets:
            self.send_error_xml(404, "NoSuchBucket",
                                "The specified bucket does not exist.")
            return
        if not STORE.delete(bucket, key):
            self.send_error_xml(404, "NoSuchKey",
                                "The specified key does not exist.")
            return
        self.send_bytes(204, b"")


def main():
    port = int(os.environ.get("LSCP_TEST_PORT", "0"))
    cert = HERE / "fixtures" / "s3-server-cert.pem"
    key = HERE / "fixtures" / "s3-server-key.pem"
    if not cert.is_file() or not key.is_file():
        print("MISSING-FIXTURES %s" % (HERE / "fixtures"), flush=True)
        return 1
    httpd = ThreadingHTTPServer(("127.0.0.1", port), S3Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=str(cert), keyfile=str(key))
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
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
