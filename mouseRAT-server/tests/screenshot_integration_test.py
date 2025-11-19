import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time

from multiclient_test import Server, complete, frame, output, receive_frame


BMP = bytes.fromhex(
    '424d3a00000000000000360000002800000001000000010000000100180000000000'
    '04000000130b0000130b000000000000000000000000ff00')
PNG = bytes.fromhex(
    '89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489'
    '0000000d49444154789c63f8cfc0f01f00050001ff89993d1d0000000049454e44ae426082')


def begin(request_id, data, encoding=2):
    return frame(0x21, request_id, struct.pack('!QI', len(data), encoding))


def screenshot(server, peer, client_id=1):
    server.command(f':use {client_id}'.encode())
    server.expect(f'Selected client {client_id}'.encode())
    server.command(b'screenshot')
    kind, request_id, payload = receive_frame(peer)
    assert kind == 0x20 and request_id and payload == b''
    return request_id


def wait_file(path):
    deadline = time.monotonic() + 5
    while not path.exists():
        assert time.monotonic() < deadline, path
        time.sleep(.01)


def saved(server, root, client_id, request_id, transfer_id, data, suffix):
    server.expect(f'[client {client_id} request {request_id}] saved '.encode())
    files = list(root.glob(f'screenshots/run-*/capture-{transfer_id}.{suffix}'))
    assert len(files) == 1 and files[0].read_bytes() == data, files


def normal():
    with tempfile.TemporaryDirectory(prefix='mouserat-images-') as directory:
        root = Path(directory)
        server = Server(cwd=directory)
        try:
            server.expect(b'Controls:')
            first, second = server.connect(1), server.connect(2)
            request_id = screenshot(server, first)
            header = begin(request_id, BMP)
            first.sendall(header[:7])
            server.command(b':use 2')
            server.expect(b'Selected client 2')
            server.command(b'other-client')
            other = server.receive(second, b'other-client')
            second.sendall(output(other, b'other-progress\n') + complete(other))
            server.expect(b'other-progress\n')
            first.sendall(header[7:] + frame(0x22, request_id, BMP[:1]) +
                          frame(0x22, request_id, BMP[1:]) + complete(request_id))
            saved(server, root, 1, request_id, 1, BMP, 'bmp')

            request_id = screenshot(server, first)
            first.sendall(begin(request_id, PNG, 1) + frame(0x22, request_id, PNG) +
                          complete(request_id) + frame(0x30, 0, b'12345678'))
            assert receive_frame(first) == (0x31, 0, b'12345678')
            saved(server, root, 1, request_id, 2, PNG, 'png')

            request_id = screenshot(server, first)
            first.sendall(begin(request_id, BMP) + frame(0x22, request_id, BMP[:2]) +
                          frame(0x7f, request_id, struct.pack('!I', 3) + b'capture failed'))
            server.expect(f'[client 1 request {request_id}] screenshot canceled'.encode())

            for client_id, malformed in [(3, False), (4, True)]:
                peer = server.connect(client_id)
                request_id = screenshot(server, peer, client_id)
                peer.sendall(begin(request_id, BMP) + frame(0x22, request_id,
                             b'xx' if malformed else BMP[:2]))
                if malformed:
                    peer.sendall(complete(request_id))
                else:
                    peer.sendall(frame(0x30, 0, b'begun-ok'))
                    assert receive_frame(peer) == (0x31, 0, b'begun-ok')
                    peer.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    peer.close()
                server.expect(f'[client {client_id}] disconnected'.encode())
                server.expect(f'[client {client_id} request {request_id}] screenshot canceled'.encode())
            assert len(list(root.glob('screenshots/run-*/capture-*.*'))) == 2
            print('PASS: live PNG/BMP saving, framing, other-client progress, ERROR and disconnect cleanup')
        finally:
            server.close()
        assert not list(root.rglob('*.part'))


def backpressure(disconnect):
    with tempfile.TemporaryDirectory(prefix='mouserat-image-pressure-') as directory:
        root = Path(directory)
        gate = root / 'disk'
        server = Server(cwd=directory, env={**os.environ, 'MOUSERAT_TEST_DISK_GATE': str(gate)})
        try:
            server.expect(b'Controls:')
            first, second = server.connect(1), server.connect(2)
            request_id = screenshot(server, first)
            data = b'BM' + bytes(range(200))
            first.sendall(begin(request_id, data) + b''.join(frame(0x22, request_id, bytes([byte]))
                                                           for byte in data) + complete(request_id))
            wait_file(root / 'disk.blocked')
            wait_file(root / 'disk.paused')
            assert not list(root.rglob('*.bmp'))
            server.command(b':use 2')
            server.expect(b'Selected client 2')
            server.command(b'while-disk-blocked')
            other = server.receive(second, b'while-disk-blocked')
            second.sendall(output(other, b'loop-still-runs\n') + complete(other))
            server.expect(b'loop-still-runs\n')
            if disconnect:
                first.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                first.close()
                server.expect(b'[client 1] disconnected')
            else:
                first.shutdown(socket.SHUT_WR)
            (root / 'disk.release').touch()
            if disconnect:
                server.expect(f'[client 1 request {request_id}] screenshot canceled'.encode())
                assert not list(root.rglob('*.bmp'))
            else:
                saved(server, root, 1, request_id, 1, data, 'bmp')
            print('PASS: full disk queue, retained coalesced input and ' +
                  ('paused-client reset cleanup' if disconnect else 'read resumption'))
        finally:
            (root / 'disk.release').touch()
            server.close()
        assert not list(root.rglob('*.part'))


def storage_failure(operation):
    with tempfile.TemporaryDirectory(prefix='mouserat-image-error-') as directory:
        root = Path(directory)
        if operation == 'begin':
            (root / 'screenshots').write_text('preserve existing file')
        server = Server(cwd=directory, env={**os.environ, 'MOUSERAT_TEST_DISK_FAIL': operation})
        try:
            server.expect(b'Controls:')
            peer, healthy = server.connect(1), server.connect(2)
            request_id = screenshot(server, peer)
            peer.sendall(begin(request_id, BMP) + frame(0x22, request_id, BMP) + complete(request_id))
            server.expect(b'storage error: ')
            server.expect(b'[client 1] disconnected')
            server.expect(f'[client 1 request {request_id}] screenshot canceled'.encode())
            assert not list(root.rglob('*.bmp'))
            server.command(b':use 2')
            server.expect(b'Selected client 2')
            server.command(b'after-storage-error')
            other = server.receive(healthy, b'after-storage-error')
            healthy.sendall(output(other, b'healthy-after-storage-error\n') + complete(other))
            server.expect(b'healthy-after-storage-error\n')
            print(f'PASS: {operation} failure isolation and cleanup')
        finally:
            server.close()
        assert not list(root.rglob('*.part'))


if __name__ == '__main__':
    normal()
    storage_failure('begin')
    if '--faults' in sys.argv[2:]:
        backpressure(False)
        backpressure(True)
        storage_failure('write')
        storage_failure('fsync')
