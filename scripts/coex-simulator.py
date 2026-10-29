#!/usr/bin/env python3
"""A stand-in NovaStar COEX controller, for trying the Hardware Map without one.

Answers the reads the Director's processor monitor makes - input sources,
screens, monitoring - and accepts EDID writes, on the COEX HTTP port. It is a
development aid, not an emulator: it models one All-In-One controller with
four inputs and one screen whose two layers take the left and right halves of
inputs one and two.

    python scripts/coex-simulator.py                      # 127.0.0.1:8001
    python scripts/coex-simulator.py --port 18001 --edid-rate 60

In Hardware Map, give a NovaStar processor the address 127.0.0.1 (or
127.0.0.1:18001), or press Find processors: the search tries this machine
too. Standard library only.
"""
from __future__ import annotations

import argparse
import json
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Controller:
    def __init__(self, name: str, edid_rate: float, signal_rate: float,
                 width: int, height: int) -> None:
        self.lock = threading.Lock()
        self.name = name
        self.inputs = []
        for number, (kind, label, live) in enumerate(
                ((3, 'HDMI 2.0-1', True), (3, 'HDMI 2.0-2', True),
                 (5, 'DP 1.2', False), (9, '12G-SDI', False)), start=1):
            self.inputs.append({
                'id': number, 'type': kind, 'name': label,
                'groupId': 9 + number, 'usable': True,
                'sourceStatus': 1 if live else 0,
                'isSupportEDID': kind != 9,
                'actualResolution': {'width': width, 'height': height},
                'actualRefreshRate': signal_rate,
                'defaultEDID': {'resolution': {'width': width, 'height': height},
                                'refreshRate': edid_rate},
            })
        self.screen_width = 6912
        self.screen_height = 1536

    def sources(self):
        with self.lock:
            return json.loads(json.dumps(self.inputs))

    def screens(self):
        half = self.screen_width // 2
        cabinets = [
            {'cabinetID': index + 1, 'connectID': 0, 'outputID': 1 + index // 9,
             'position': {'x': (index % 9) * 384, 'y': (index // 9) * 384},
             'size': {'width': 384, 'height': 384}, 'angle': 0,
             'lockStatus': False}
            for index in range(18 * 4 // 2)
        ]
        canvases = [
            {'canvasID': 0, 'outputCardId': 0, 'canvasSerialNum': 0,
             'ordinal': 0, 'size': {'width': half, 'height': self.screen_height},
             'isCustomSize': False, 'position': {'x': 0, 'y': 0},
             'rectSize': {'width': half, 'height': self.screen_height},
             'cabinets': cabinets, 'groups': [], 'maxFrameRate': 120,
             'layoutLines': [], 'zorder': 0, 'frequencyPhaseStatus': 0},
            {'canvasID': 1, 'outputCardId': 1, 'canvasSerialNum': 1,
             'ordinal': 1, 'size': {'width': half, 'height': self.screen_height},
             'isCustomSize': False, 'position': {'x': half, 'y': 0},
             'rectSize': {'width': half, 'height': self.screen_height},
             'cabinets': cabinets, 'groups': [], 'maxFrameRate': 120,
             'layoutLines': [], 'zorder': 1, 'frequencyPhaseStatus': 0},
        ]
        layers = [
            {'id': index + 1, 'layerIndex': index, 'source': 10 + index,
             'position': {'x': index * half, 'y': 0}, 'zOrder': index,
             'lock': False,
             'border': {'enable': False, 'width': 0,
                        'color': {'r': 0, 'g': 0, 'b': 0}},
             'cut': {'enable': True,
                     'rect': {'x': 0, 'y': 0, 'width': half,
                              'height': self.screen_height}},
             'scaler': {'width': half, 'height': self.screen_height},
             'followState': False, 'layerInCanvasId': index}
            for index in range(2)
        ]
        return {
            'screens': [{
                'screenID': '{6b1e2f30-0000-4000-8000-000000000001}',
                'screenName': 'Screen1', 'createTime': '', 'workingMode': 1,
                'lowLatency': False, 'layoutMode': 0, 'screenIndex': 0,
                'screenGroupID': '', 'ordinal': 0,
                'position': {'x': 0, 'y': 0}, 'canvases': canvases,
                'layersInWorkingMode': [
                    {'workingMode': 0, 'layerLayoutMode': 0, 'layers': []},
                    {'workingMode': 1, 'layerLayoutMode': 0, 'layers': layers},
                ],
                'masterFrameRate': 59.94, 'pixToPixMode': 0,
            }],
            'screenGroups': [],
        }

    def write_edid(self, input_id: int, body: dict) -> str | None:
        mode = body.get('para', body)
        resolution = mode.get('resolution') or {}
        width = int(resolution.get('width', 0))
        height = int(resolution.get('height', 0))
        rate = float(mode.get('refreshRate', 0.0))
        if width <= 0 or height <= 0 or rate <= 0.0:
            return 'InvalidParam'
        with self.lock:
            for source in self.inputs:
                if source['id'] == input_id:
                    if not source['isSupportEDID']:
                        return 'NotSupport'
                    source['defaultEDID'] = {
                        'resolution': {'width': width, 'height': height},
                        'refreshRate': rate}
                    # A source re-reads the EDID and follows it.
                    if source['sourceStatus']:
                        source['actualResolution'] = {'width': width,
                                                      'height': height}
                        source['actualRefreshRate'] = rate
                    return None
        return 'InvalidParam'


def make_handler(controller: Controller):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'COEX-simulator/1'

        def answer(self, data=None, code=0, message='Success', status=200):
            body = json.dumps({'code': code, 'data': data,
                               'message': message}).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):  # noqa: N802 - the stdlib's name
            if self.path == '/api/v1/device/input/sources':
                self.answer(controller.sources())
            elif self.path == '/api/v1/screen':
                self.answer(controller.screens())
            elif self.path == '/api/v1/device/monitor/info':
                self.answer({'name': controller.name, 'cabinets': [],
                             'fanInfos': [], 'mainBoardTemperature': {'value': 41}})
            else:
                self.answer(None, 36, 'NotImplement', 404)

        def do_PUT(self):  # noqa: N802
            length = int(self.headers.get('Content-Length', '0') or 0)
            try:
                body = json.loads(self.rfile.read(length) or b'{}')
            except json.JSONDecodeError:
                self.answer(None, 4, 'AnalysisFailed')
                return
            match = re.fullmatch(r'/api/v1/device/input/(\d+)/edid', self.path)
            if not match:
                self.answer(None, 36, 'NotImplement', 404)
                return
            error = controller.write_edid(int(match.group(1)), body)
            if error:
                self.answer(None, 1, error)
            else:
                print(f'EDID on input {match.group(1)} set to '
                      f'{json.dumps(body.get("para", body))}', flush=True)
                self.answer(None)

        def log_message(self, format, *args):  # noqa: A002
            pass

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8001)
    parser.add_argument('--name', default='MX40 Pro_000001')
    parser.add_argument('--edid-rate', type=float, default=60.0,
                        help='refresh the inputs EDIDs offer (Hz)')
    parser.add_argument('--signal-rate', type=float, default=60.0,
                        help='refresh the live inputs report receiving (Hz)')
    parser.add_argument('--width', type=int, default=3840)
    parser.add_argument('--height', type=int, default=2160)
    args = parser.parse_args()
    controller = Controller(args.name, args.edid_rate, args.signal_rate,
                            args.width, args.height)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(controller))
    print(f'COEX simulator "{args.name}" on http://{args.host}:{args.port}',
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
