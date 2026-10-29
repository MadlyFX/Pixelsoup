#!/usr/bin/env python3
"""A stand-in Brompton Tessera processor, for trying the Hardware Map without one.

Answers the IP control reads the Director's processor monitor makes - model,
software, the active input, panel counts, overrides and the active preset -
under /api, as Tessera 3.1 does on HTTP port 80. Values can be changed with a
PUT of {"data": value}, as on a real processor, so an override or a dropped
panel can be shown on the map. A development aid, not an emulator.

    python scripts/tessera-simulator.py                   # 127.0.0.1:80
    python scripts/tessera-simulator.py --port 8080 --rate 59.94
    curl -X PUT -d "{\\"data\\": true}" http://127.0.0.1:8080/api/override/blackout/enabled

Port 80 may be taken, or need elevation; with another port, give the card the
address 127.0.0.1:8080 (Find processors only looks on port 80). Standard
library only.
"""
from __future__ import annotations

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def make_tree(args) -> dict:
    return {
        'system/processor-type': args.model,
        'system/software-version': '3.1.0',
        'system/uptime': '00d 01h 00m 00s',
        'input/active/source/port-type': args.port_type,
        'input/active/source/port-number': 1,
        'input/active/resolution/width': args.width,
        'input/active/resolution/height': args.height,
        'input/active/refresh-rate': args.rate,
        'panels/statistics/associated-count': args.panels,
        'panels/statistics/online-count': args.panels,
        'panels/statistics/error-count': 0,
        'override/blackout/enabled': False,
        'override/freeze/enabled': False,
        'override/test-pattern/enabled': False,
        'presets/active/name': 'Show',
        'presets/active/number': '1',
        'output/global-colour/brightness': 1500.0,
    }


def make_handler(tree: dict, lock: threading.Lock):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'Tessera-simulator/1'

        def key(self) -> str | None:
            path = self.path.split('?', 1)[0].strip('/').lower()
            if not path.startswith('api/'):
                return None
            return path[4:]

        def answer(self, status: int, body) -> None:
            data = json.dumps(body).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):  # noqa: N802 - the stdlib's name
            key = self.key()
            with lock:
                if key is None or key not in tree:
                    self.answer(404, {'error': 'Path not found'})
                    return
                self.answer(200, {key.rsplit('/', 1)[-1]: tree[key]})

        def do_PUT(self):  # noqa: N802
            key = self.key()
            length = int(self.headers.get('Content-Length', '0') or 0)
            try:
                body = json.loads(self.rfile.read(length) or b'{}')
            except json.JSONDecodeError:
                self.answer(400, {'error': 'Missing input parameter'})
                return
            if 'data' not in body:
                self.answer(400, {'error': 'Missing input parameter'})
                return
            with lock:
                if key is None or key not in tree:
                    self.answer(404, {'error': 'Path not found'})
                    return
                tree[key] = body['data']
                print(f'{key} = {body["data"]}', flush=True)
                self.answer(200, {key.rsplit('/', 1)[-1]: tree[key]})

        def log_message(self, format, *args):  # noqa: A002
            pass

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=80)
    parser.add_argument('--model', default='s8',
                        choices=['m2', 's4', 's8', 't1', 't8', 'sx40'])
    parser.add_argument('--port-type', default='hdmi',
                        choices=['dvi', 'hdmi', 'sdi'])
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--rate', type=float, default=60.0,
                        help='refresh the active input reports (Hz)')
    parser.add_argument('--panels', type=int, default=64)
    args = parser.parse_args()
    tree = make_tree(args)
    server = ThreadingHTTPServer((args.host, args.port),
                                 make_handler(tree, threading.Lock()))
    print(f'Tessera simulator ({args.model.upper()}) on '
          f'http://{args.host}:{args.port}/api', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
