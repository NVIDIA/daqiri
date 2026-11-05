# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
"""Verify real AWS SDK PutObject bodies against a localhost-only HTTP fixture."""
import http.server
import io
import os
import subprocess
import sys
import threading


def chunks(stream):
    result = bytearray()
    while True:
        length = int(stream.readline().split(b";", 1)[0].strip(), 16)
        if length == 0:
            while stream.readline().strip():
                pass
            return bytes(result)
        result.extend(stream.read(length))
        assert stream.read(2) == b"\r\n"


received = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_PUT(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = chunks(self.rfile)
        else:
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        decoded = int(self.headers.get("x-amz-decoded-content-length", len(body)))
        if "aws-chunked" in self.headers.get("Content-Encoding", "") and len(body) != decoded:
            body = chunks(io.BytesIO(body))
        received.append((self.path, body))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.send_header("ETag", '"local-test"')
        self.end_headers()


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
env = os.environ.copy()
env.update(AWS_ACCESS_KEY_ID="local-test", AWS_SECRET_ACCESS_KEY="local-test",
           AWS_EC2_METADATA_DISABLED="true", AWS_MAX_ATTEMPTS="1")
try:
    subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"],
                   env=env, check=True, timeout=75)
    expected = bytes([0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 3, 4, 5])
    bodies = {f"/null-writer-test/{int(all_null)}-{offset}_0":
              (bytes(13) if all_null else expected)[offset:]
              for all_null in (False, True) for offset in (0, 2, 6, 13)}
    assert len(received) == len(bodies), received
    assert dict(received) == bodies, received
    print("PASS: AWS SDK PutObject received exact zero-filled null-span bodies")
finally:
    server.shutdown()
    server.server_close()
    thread.join()
