import hashlib
from pathlib import Path
import struct
import tempfile

from multiclient_test import HEADER, Server, complete, frame, receive_frame
from screenshot_integration_test import BMP, begin, saved, screenshot


with tempfile.TemporaryDirectory(prefix='mouserat-image-limits-') as directory:
    root = Path(directory)
    server = Server(cwd=directory)
    try:
        server.expect(b'Controls:')
        baseline_fds = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
        large, small = server.connect(1), server.connect(2)
        large.settimeout(10)
        large_id = screenshot(server, large)
        small_id = screenshot(server, small, 2)

        chunk = (b'BM' + bytes(range(256)) * 256)[:65536]
        small_image = chunk + b'last-bytes'
        small.sendall(begin(small_id, small_image) + frame(0x22, small_id, chunk) +
                      frame(0x30, 0, b'small-ok'))
        assert receive_frame(small) == (0x31, 0, b'small-ok')
        large.sendall(frame(0x21, large_id, struct.pack('!QI', 32 * 1024 * 1024, 2)) +
                      frame(0x22, large_id, chunk) + frame(0x30, 0, b'large-ok'))
        assert receive_frame(large) == (0x31, 0, b'large-ok')

        expected = hashlib.sha256(chunk)
        for index in range(1, 512):
            large.sendall(frame(0x22, large_id, chunk))
            expected.update(chunk)
            if index == 256:
                small.sendall(frame(0x22, small_id, b'last-bytes') + complete(small_id))
                saved(server, root, 2, small_id, 1, small_image, 'bmp')
                assert not list(root.glob('screenshots/run-*/capture-2.bmp'))

        large.sendall(complete(large_id))
        server.expect(f'[client 1 request {large_id}] saved '.encode())
        files = list(root.glob('screenshots/run-*/capture-2.bmp'))
        assert len(files) == 1 and files[0].stat().st_size == 32 * 1024 * 1024
        actual = hashlib.sha256()
        with files[0].open('rb') as image:
            for data in iter(lambda: image.read(65536), b''):
                actual.update(data)
        assert actual.digest() == expected.digest()

        oversized = server.connect(3)
        request_id = screenshot(server, oversized, 3)
        oversized.sendall(frame(0x21, request_id, struct.pack('!QI', 32 * 1024 * 1024 + 1, 2)))
        server.expect(b'[client 3] disconnected')
        oversized.close()

        for client_id, oversized_frame in [(4, True), (5, False)]:
            peer = server.connect(client_id)
            request_id = screenshot(server, peer, client_id)
            peer.sendall(begin(request_id, BMP) + frame(0x30, 0, b'begun-ok'))
            assert receive_frame(peer) == (0x31, 0, b'begun-ok')
            if oversized_frame:
                peer.sendall(HEADER.pack(b'MRAT', 1, 0x22, 0, 65537, request_id))
            else:
                peer.sendall(frame(0x22, request_id, BMP + b'x'))
            server.expect(f'[client {client_id}] disconnected'.encode())
            server.expect(f'[client {client_id} request {request_id}] screenshot canceled'.encode())
            peer.close()

        assert not list(root.rglob('*.part'))
        assert len(list(root.rglob('*.bmp'))) == 2
        assert len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == baseline_fds + 2
        print('PASS: 64 KiB frames, 32 MiB image, overlapping captures, admission order, limits and FD cleanup')
    finally:
        server.close()
