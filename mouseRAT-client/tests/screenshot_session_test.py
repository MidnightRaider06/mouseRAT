"""Run a native Windows client against a local framed mock server, with Pillow."""

import ctypes
from ctypes import wintypes
import io
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

from PIL import Image


def frame(kind, request_id=0, payload=b''):
    return struct.pack('!4sBBHII', b'MRAT', 1, kind, 0, len(payload), request_id) + payload


def receive(peer):
    def exact(count):
        data = bytearray()
        while len(data) < count:
            part = peer.recv(min(count - len(data), 4096))
            assert part, 'Unexpected EOF'
            data.extend(part)
        return data

    magic, version, kind, flags, count, request_id = struct.unpack('!4sBBHII', exact(16))
    assert magic == b'MRAT' and version == 1 and flags == 0 and count <= 65536
    return kind, request_id, bytes(exact(count))


def connect(listener, executable):
    process = subprocess.Popen([executable, '127.0.0.1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        peer, _ = listener.accept()
        peer.settimeout(10)
        peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        assert receive(peer) == (1, 0, b'')
        ack = frame(2)
        peer.sendall(ack[:5])
        peer.sendall(ack[5:])
        return process, peer
    except BaseException:
        process.kill()
        process.communicate()
        raise


def stop(process, peer, expected):
    peer.close()
    try:
        output, error = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise
    assert process.returncode == expected, (process.returncode, output, error)
    if sys.exc_info()[0] is not None:
        print(output.decode(errors='replace'), error.decode(errors='replace'), file=sys.stderr)


def screenshot(peer, request_id, dimensions, origin, fixture, slow=False, capture_error=False):
    nonce = struct.pack('!Q', request_id)
    request = frame(0x20, request_id)
    peer.sendall(request[:7])
    peer.sendall(request[7:] + frame(0x30, payload=nonce))
    total = None
    image = bytearray()
    pong = False
    while True:
        kind, response_id, payload = receive(peer)
        if kind == 0x30:
            assert response_id == 0 and len(payload) == 8
            peer.sendall(frame(0x31, payload=payload))
        elif kind == 0x31:
            assert response_id == 0 and payload == nonce and not pong
            pong = True
        elif kind == 0x21:
            assert response_id == request_id and total is None and len(payload) == 12
            total, encoding = struct.unpack('!QI', payload)
            assert encoding == 2 and 0 < total <= 32 * 1024 * 1024
            if slow:
                time.sleep(.3)
        elif kind == 0x22:
            assert response_id == request_id and total is not None and payload
            image.extend(payload)
            assert len(image) <= total
        elif kind == 0x7f and capture_error:
            assert response_id == request_id and payload[:4] == bytes([0, 0, 0, 3])
            assert total is None and not image and pong
            return
        else:
            assert not capture_error, 'Capture unexpectedly succeeded'
            assert (kind, response_id, payload) == (0x12, request_id, bytes(4)), (kind, response_id, payload)
            assert len(image) == total and pong
            break
    assert image[:2] == b'BM' and struct.unpack_from('<I', image, 2)[0] == total
    with Image.open(io.BytesIO(image)) as decoded:
        decoded.load()
        assert decoded.format == 'BMP' and decoded.size == dimensions
        if fixture:
            white = decoded.getpixel((50 - origin[0], 50 - origin[1]))
            black = decoded.getpixel((70 - origin[0], 70 - origin[1]))
            # Desktop color management can adjust the fixture's system colors
            assert all(abs(actual - expected) <= 16 for actual, expected in
                       zip(white + black, fixture[0] + fixture[1])), (white, black, fixture)


def main():
    executable = str(Path(sys.argv[1]).resolve(strict=True))
    capture_error = '--expect-capture-error' in sys.argv[2:]
    user = ctypes.WinDLL('user32', use_last_error=True)
    user.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    previous = user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-3))
    assert previous
    dimensions = tuple(user.GetSystemMetrics(index) for index in (78, 79))
    origin = tuple(user.GetSystemMetrics(index) for index in (76, 77))
    colors = [user.GetSysColor(index) for index in (5, 6)]
    fixture = tuple(tuple((color >> shift) & 255 for shift in (0, 8, 16)) for color in colors)
    user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                   wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE,
                                   ctypes.c_void_p]
    user.CreateWindowExW.restype = wintypes.HWND
    user.UpdateWindow.argtypes = user.DestroyWindow.argtypes = [wintypes.HWND]
    user.GetGuiResources.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    white = user.CreateWindowExW(0x08000008, 'STATIC', 'Capture test', 0x90000006,
                               40, 40, 96, 64, None, None, None, None)
    assert white
    try:
        black = user.CreateWindowExW(0, 'STATIC', '', 0x50000004, 20, 20, 40, 20,
                                   white, None, None, None)
        assert black
        user.UpdateWindow(white)
        user.UpdateWindow(black)
        ctypes.WinDLL('dwmapi').DwmFlush()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 5555))
            listener.listen(2)
            listener.settimeout(5)
            process, peer = connect(listener, executable)
            try:
                for request_id in range(1, 5):
                    screenshot(peer, request_id, dimensions, origin, fixture, slow=request_id == 1,
                               capture_error=capture_error)
                    count = user.GetGuiResources(wintypes.HANDLE(int(process._handle)), 0)
                    if request_id == 1:
                        baseline = count
                    assert count == baseline, ('GDI handle growth', baseline, count)
                peer.sendall(frame(0x10, 5, b'echo screenshot'))
                output = bytearray()
                while True:
                    kind, request_id, payload = receive(peer)
                    assert request_id == 5
                    if kind == 0x12:
                        assert payload == bytes(4) and output == b'screenshot\r\n'
                        break
                    assert kind == 0x11 and payload[:1] == b'\0'
                    output.extend(payload[1:])
            finally:
                stop(process, peer, 0)
            if capture_error:
                print('PASS: capture failure reports ERROR 3, releases GDI resources and permits another request')
                return
            print(f'PASS: native BMP {dimensions}, pixels, repeated capture, heartbeats and command/type dispatch')

            process, peer = connect(listener, executable)
            peer.sendall(frame(0x20, 1))
            assert receive(peer)[0] == 0x21
            stop(process, peer, 1)
            print('PASS: disconnect during image transfer releases the client session')

            process, peer = connect(listener, executable)
            peer.sendall(frame(0x20, 1) + frame(0x20, 2))
            try:
                process.wait(timeout=5)
            finally:
                stop(process, peer, 1)
            print('PASS: overlapping screenshot requests close the session')

            first, a = connect(listener, executable)
            second, b = connect(listener, executable)
            try:
                screenshot(a, 1, dimensions, origin, fixture)
                screenshot(b, 1, dimensions, origin, fixture)
            finally:
                try:
                    stop(first, a, 0)
                finally:
                    stop(second, b, 0)
            print('PASS: two Windows clients complete independent screenshot sessions')
    finally:
        user.DestroyWindow(white)
        user.SetThreadDpiAwarenessContext(previous)


if __name__ == '__main__':
    main()
