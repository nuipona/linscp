#!/usr/bin/env python3
"""Offline tests for desktop-file upload staging and task construction."""

import http.client
import io
import json
import os
import pathlib
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock
from http.server import ThreadingHTTPServer

GUI_DIR = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_DIR))
import linuxscp_gui as gui  # noqa: E402


class FakeTasks:
    def __init__(self):
        self.args = None
        self.submitted = []
        self.task = types.SimpleNamespace(id=5)

    def submit(self, *args, **kwargs):
        self.args = (args, kwargs)
        self.submitted.append(self.args)
        self.task.cleanup_path = kwargs.get("cleanup_path")
        return self.task


class BrowserUploadTests(unittest.TestCase):
    def setUp(self):
        self.tasks = FakeTasks()
        self.original_tasks = gui.TASKS
        gui.TASKS = self.tasks
        self.tab = types.SimpleNamespace(
            id=7, session={"protocol": "sftp"}, remote_dir="/default")

    def tearDown(self):
        gui.TASKS = self.original_tasks
        for _, kwargs in self.tasks.submitted:
            path = kwargs.get("cleanup_path")
            if path:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def test_upload_stages_bytes_and_queues_the_exact_remote_path(self):
        stream = io.BytesIO(b"binary\x00payload")
        task = gui.queue_browser_upload(
            self.tab, "/incoming files", "note \"one\".txt", stream, 14)
        args, kwargs = self.tasks.args
        self.assertIs(task, self.tasks.task)
        self.assertEqual(args[0], "Upload")
        self.assertIn("mkdir -parents ", args[1])
        self.assertIn("put ", args[1])
        self.assertEqual(args[3], "Browser: note \"one\".txt")
        self.assertEqual(args[4], "/incoming files/note \"one\".txt")
        self.assertEqual(args[5], 7)
        self.assertEqual(kwargs["progress_remote"],
                         (7, "/incoming files/note \"one\".txt"))
        self.assertEqual(kwargs["cleanup_path"], self.tasks.task.cleanup_path)
        self.assertEqual(pathlib.Path(kwargs["cleanup_path"]).read_bytes(),
                         b"binary\x00payload")
        self.assertEqual(self.tasks.task.bytes_total, 14)

    def test_folder_entries_keep_relative_paths_and_queue_empty_directories(self):
        directory = gui.queue_browser_directory(
            self.tab, "/incoming files", "Project/empty folder")
        self.assertIs(directory, self.tasks.task)
        args, kwargs = self.tasks.args
        self.assertEqual(args[0], "Create directory")
        self.assertEqual(args[1], 'mkdir -parents "/incoming files/Project/empty folder"')
        self.assertEqual(args[4], "/incoming files/Project/empty folder")
        self.assertFalse(kwargs.get("cleanup_path"))

        file = 'daily %TIMESTAMP%.txt'
        gui.queue_browser_upload(self.tab, "/incoming files", file,
                                 io.BytesIO(b"abc"), 3,
                                 "Project/notes/" + file)
        args, kwargs = self.tasks.args
        self.assertEqual(args[4], "/incoming files/Project/notes/" + file)
        self.assertIn('mkdir -parents "/incoming files/Project/notes" ; put ',
                      args[1])
        self.assertIn('"/incoming files/Project/notes/' + file + '"', args[1])
        self.assertEqual(args[3], "Browser: Project/notes/" + file)
        self.assertEqual(pathlib.Path(kwargs["cleanup_path"]).read_bytes(), b"abc")

    def test_gui_queue_uses_literal_commands_for_percent_filenames(self):
        # Unlike run_linuxscp, the background queue builds a fresh command
        # line. Do not let it expand a dropped %TIMESTAMP% as a script token.
        task = types.SimpleNamespace(
            kind="Upload", command='put /tmp/file "/remote/%TIMESTAMP%.txt"',
            session={}, source="file", status="Queued")
        manager = gui.TaskManager.__new__(gui.TaskManager)
        with mock.patch.object(gui, "core_command_line", return_value=["linuxscp"]), \
             mock.patch.object(gui, "session_url", return_value="sftp://host/"), \
             mock.patch.object(gui, "add_log"), \
             mock.patch.object(gui.subprocess, "Popen",
                               side_effect=OSError("stub")) as popen:
            manager._run(task)
        command = popen.call_args.args[0]
        self.assertIn("-literal", command)
        self.assertIn("/remote/%TIMESTAMP%.txt", command[-1])
        self.assertEqual(task.status, "Failed")

    def test_dropped_paths_are_validated_before_staging_or_queueing(self):
        for path in ("../escape.txt", "/absolute.txt", "folder/../escape.txt",
                     "folder//file.txt", "folder/./file.txt", "folder/",
                     "folder\\file.txt", "folder/hello\n.txt",
                     "folder/\ud800.txt", "/".join(["nest"] * 33) + "/file.txt"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                gui.browser_destination("/incoming", path)
        for remote_dir in ("relative", "/incoming/../other", "/wrong\nname"):
            with self.subTest(dir=remote_dir), self.assertRaises(ValueError):
                gui.browser_destination(remote_dir, "file.txt")
        self.assertEqual(gui.browser_destination("/incoming/", "Project/n.txt"),
                         "/incoming/Project/n.txt")
        with self.assertRaises(ValueError):
            gui.queue_browser_upload(self.tab, "/incoming", "different.txt",
                                     io.BytesIO(b"secret"), 6,
                                     "other/file.txt")
        self.assertIsNone(self.tasks.args)

    def test_http_endpoint_requires_token_and_queues_raw_file_bytes(self):
        class Tab:
            id = 77
            connected = True
            remote_dir = "/default"
            session = {"protocol": "sftp"}

        class QuietHandler(gui.LinuxScpGuiHandler):
            def log_message(self, *_args):
                pass

        old_config = (gui.API_TOKEN, gui.REQUIRE_TOKEN, gui.ALLOWED_HOSTS,
                      gui.ALLOW_ANY_HOST, gui.STATE["tabs"],
                      gui.STATE["active_tab"])
        gui.API_TOKEN = "browser-upload-test-token"
        gui.REQUIRE_TOKEN = True
        gui.ALLOWED_HOSTS = {"127.0.0.1"}
        gui.ALLOW_ANY_HOST = False
        gui.STATE["tabs"] = [Tab()]
        gui.STATE["active_tab"] = 77
        server = ThreadingHTTPServer(("127.0.0.1", 0), QuietHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        path = None
        try:
            body = b"actual HTTP file\x00bytes"
            target = ("/api/transfer/drop?tab=77&remote_dir=%2Fincoming"
                      "&name=desktop.bin")
            denied = http.client.HTTPConnection("127.0.0.1", server.server_port)
            denied.request("POST", target, body,
                           {"Content-Type": "application/octet-stream"})
            response = denied.getresponse()
            response.read()
            self.assertEqual(response.status, 401)
            denied.close()
            self.assertIsNone(self.tasks.args)

            conn = http.client.HTTPConnection("127.0.0.1", server.server_port)
            conn.request("POST", target, body,
                         {"X-LSCP-Token": gui.API_TOKEN,
                          "Content-Type": "application/octet-stream"})
            response = conn.getresponse()
            payload = response.read()
            conn.close()
            self.assertEqual(response.status, 200, payload)
            self.assertEqual(json.loads(payload.decode("utf-8"))["task_id"], 5)
            args, kwargs = self.tasks.args
            path = kwargs["cleanup_path"]
            self.assertEqual(pathlib.Path(path).read_bytes(), body)
            self.assertEqual(args[4], "/incoming/desktop.bin")

            def post(url, data, mime, token=True):
                conn = http.client.HTTPConnection("127.0.0.1", server.server_port)
                headers = {"Content-Type": mime}
                if token:
                    headers["X-LSCP-Token"] = gui.API_TOKEN
                conn.request("POST", url, data, headers)
                response = conn.getresponse()
                code, result = response.status, response.read()
                conn.close()
                return code, json.loads(result.decode("utf-8"))

            directory = json.dumps({"tab": 77, "remote_dir": "/incoming",
                                    "relative_path": "Folder/empty"}).encode()
            code, _ = post("/api/transfer/drop-dir", directory,
                           "application/json", token=False)
            self.assertEqual(code, 401)
            code, result = post("/api/transfer/drop-dir", directory,
                                "application/json")
            self.assertEqual(code, 200, result)
            self.assertEqual(self.tasks.args[0][4], "/incoming/Folder/empty")
            self.assertEqual(self.tasks.args[0][1],
                             "mkdir -parents /incoming/Folder/empty")
            before = len(self.tasks.submitted)
            invalid = json.dumps({"tab": 77, "remote_dir": "/incoming",
                                  "relative_path": "Folder/../outside"}).encode()
            code, result = post("/api/transfer/drop-dir", invalid,
                                "application/json")
            self.assertEqual(code, 400, result)
            code, result = post("/api/transfer/drop-dir", b"[]",
                                "application/json")
            self.assertEqual(code, 400, result)
            code, result = post(
                "/api/transfer/drop?tab=77&remote_dir=%2Fincoming"
                "&name=desktop.bin&relative_path=..%2Fdesktop.bin", body,
                "application/octet-stream")
            self.assertEqual(code, 400, result)
            code, result = post(
                "/api/transfer/drop?tab=999&remote_dir=%2Fincoming"
                "&name=desktop.bin", body, "application/octet-stream")
            self.assertEqual(code, 400, result)
            self.assertEqual(len(self.tasks.submitted), before)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            gui.API_TOKEN, gui.REQUIRE_TOKEN, gui.ALLOWED_HOSTS, \
                gui.ALLOW_ANY_HOST = old_config[:4]
            gui.STATE["tabs"], gui.STATE["active_tab"] = old_config[4:]
            if path:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    def test_incomplete_upload_does_not_leave_a_temporary_file_or_queue(self):
        with self.assertRaises(IOError):
            gui.queue_browser_upload(self.tab, "/incoming", "short.bin",
                                     io.BytesIO(b"short"), 20)
        self.assertIsNone(self.tasks.args)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BrowserUploadTests)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    failed = len(result.failures) + len(result.errors)
    print("evidence: suite=gui-browser-upload platform=linux ran=%d skipped=%d failed=%d" %
          (result.testsRun, len(result.skipped), failed))
    sys.exit(0 if result.wasSuccessful() else 1)
