#!/usr/bin/env python3
"""Explicit loopback server for the installed public SDK web_fetch consumer."""
import argparse
import gzip
import json
import os
from pathlib import Path
import socket
import socketserver
import threading
import time


def response(target):
    """Status, ordered headers, encoded body, delay. No remote requests."""
    plain = [('Content-Type', 'text/plain')]
    if target == '/admission/plain': return 200, plain, b'web-fetch-owned-body', 0
    if target == '/content/html':
        return 200, [('Content-Type', 'text/html')], '<style>badstyle</style><h1>Page</h1><p>hello &amp; world</p><script>SECRET_SCRIPT</script><p>中文</p>'.encode(), 0
    if target == '/content/utf8': return 200, plain, '中文'.encode(), 0
    if target == '/content/binary': return 200, plain, b'a\0b', 0
    if target == '/content/status': return 404, plain, b'not found', 0
    if target == '/limits/exact': return 200, plain, b'E' * 128, 0
    if target == '/limits/body': return 200, plain, b'E' * 129, 0
    if target == '/limits/gzip': return 200, plain + [('Content-Encoding', 'gzip')], gzip.compress(b'G' * 8192, mtime=0), 0
    if target == '/limits/header': return 200, plain + [('X-Large', 'H' * 4096)], b'', 0
    if target == '/limits/output': return 200, plain, b'U' * 1024, 0
    if target == '/limits/cumulative-body': return 302, [('Location', '/limits/body-final')], b'B' * 80, 0
    if target == '/limits/body-final': return 200, plain, b'B' * 80, 0
    if target == '/limits/cumulative-header': return 302, [('Location', '/limits/header-final')], b'', 0
    if target == '/limits/header-final': return 200, [('X-Pad', 'P' * 60)], b'OK', 0
    if target == '/redirects/relative': return 302, [('Location', 'final')], b'', 0
    if target == '/redirects/final': return 200, plain, b'relative-final', 0
    if target == '/redirects/loop-a': return 302, [('Location', '/redirects/loop-b')], b'', 0
    if target == '/redirects/loop-b': return 302, [('Location', '/redirects/loop-a')], b'', 0
    if target == '/redirects/limit': return 302, [('Location', '/redirects/unvisited')], b'', 0
    if target == '/redirects/scheme': return 302, [('Location', 'file:///not-an-http-resource')], b'', 0
    if target == '/redirects/duplicate': return 302, [('Location', '/one'), ('Location', '/two')], b'', 0
    if target in ('/cancel/wait', '/cancel/close', '/cancel/shutdown', '/cancel/timeout'):
        return 200, plain, b'too late', 15
    if target in ('/isolation/0', '/isolation/1', '/isolation/2', '/isolation/3'):
        return 200, plain, b'I' * 256, 0
    return 500, plain, b'unexpected fixture target', 0


def wire_header(status, headers, body):
    reason = {200: 'OK', 302: 'Found', 404: 'Not Found', 500: 'Internal Server Error'}.get(status, 'Status')
    text = f'HTTP/1.1 {status} {reason}\r\n'
    text += ''.join(f'{name}: {value}\r\n' for name, value in headers)
    text += f'Content-Length: {len(body)}\r\nConnection: close\r\n\r\n'
    return text.encode('ascii')


class FixtureServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = False
    block_on_close = True

    def __init__(self, requests_file):
        self.requests_file = requests_file
        self.record_lock = threading.Lock()
        self.stopping = threading.Event()
        self.failures = []
        super().__init__(('127.0.0.1', 0), Handler)

    def record(self, target, user_agent):
        if any(c in target + user_agent for c in '\t\r\n'):
            raise ValueError('fixture request record has a control delimiter')
        with self.record_lock, self.requests_file.open('ab') as output:
            output.write((target + '\t' + user_agent + '\n').encode('ascii'))
            output.flush()

    def handle_error(self, request, client_address):
        with self.record_lock:
            self.failures.append('HTTP fixture request handler failed')


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(2)
        raw = b''
        while b'\r\n\r\n' not in raw:
            block = self.request.recv(4096)
            if not block: return
            raw += block
            if len(raw) > 65536: raise ValueError('fixture request headers too large')
        lines = raw.split(b'\r\n\r\n', 1)[0].decode('ascii').split('\r\n')
        method, target, version = lines[0].split(' ')
        if method != 'GET' or version not in ('HTTP/1.0', 'HTTP/1.1'):
            raise ValueError('fixture expected an actual HTTP GET')
        headers = [line.split(':', 1) for line in lines[1:]]
        agents = [value.strip() for name, value in headers if name.lower() == 'user-agent']
        if len(agents) != 1: raise ValueError('fixture expected one explicit User-Agent')
        self.server.record(target, agents[0])
        status, headers, body, delay = response(target)
        if self.server.stopping.wait(delay): return
        try:
            self.request.sendall(wire_header(status, headers, body) + body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, socket.timeout):
            pass  # Expected when the real client cancels or enforces a byte cap.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ready-file', type=Path, required=True)
    parser.add_argument('--requests-file', type=Path, required=True)
    parser.add_argument('--stop-file', type=Path, required=True)
    args = parser.parse_args()
    paths = (args.ready_file, args.requests_file, args.stop_file)
    if any(not path.is_absolute() or path.exists() for path in paths) or len(set(paths)) != 3:
        raise ValueError('fixture paths must be distinct, absolute, fresh host-owned files')
    for path in paths: path.parent.mkdir(parents=True, exist_ok=True)
    with args.requests_file.open('xb'):
        pass
    server = FixtureServer(args.requests_file)
    serving = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.05})
    serving.start()
    try:
        ready = {'schemaVersion': 1, 'base_url': f'http://127.0.0.1:{server.server_address[1]}',
                 'pid': os.getpid(), 'requests_file': str(args.requests_file), 'stop_file': str(args.stop_file)}
        with args.ready_file.open('x', encoding='utf-8', newline='\n') as output:
            output.write(json.dumps(ready) + '\n')
            output.flush()
        while not args.stop_file.is_file():
            if not serving.is_alive(): raise RuntimeError('HTTP serving thread exited early')
            time.sleep(0.05)
    finally:
        server.stopping.set()
        server.shutdown()
        serving.join()
        server.server_close()  # Non-daemon request threads have finished here.
    if server.failures: raise RuntimeError('; '.join(server.failures))


if __name__ == '__main__':
    main()
