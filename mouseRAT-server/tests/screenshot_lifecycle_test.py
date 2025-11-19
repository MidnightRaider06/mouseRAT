import os
from pathlib import Path
import select
import sys
import tempfile
import time

from multiclient_test import Server, answer_heartbeats, complete, frame, output, receive_frame
from screenshot_integration_test import BMP, begin, screenshot, wait_file


assert len(sys.argv) >= 2, 'Pass a server linked with disk_faults.cpp'


def lifecycle(expire):
    with tempfile.TemporaryDirectory(prefix='mouserat-image-lifecycle-') as directory:
        root = Path(directory)
        gate = root / 'disk'
        server = Server(cwd=directory, env={**os.environ, 'MOUSERAT_TEST_DISK_GATE': str(gate)})
        try:
            server.expect(b'Controls:')
            baseline_fds = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
            first, second = server.connect(1), server.connect(2)
            request_id = screenshot(server, first)
            data = b'BM' + bytes(range(200))
            if expire:
                partial = server.connect(3)
                partial_id = screenshot(server, partial, 3)
                partial.sendall(begin(partial_id, BMP) + frame(0x30, 0, b'begun-ok'))
                assert receive_frame(partial) == (0x31, 0, b'begun-ok')
                partial.sendall(frame(0x22, partial_id, BMP)[:1])
                first.sendall(begin(request_id, data) + b''.join(
                    frame(0x22, request_id, bytes([byte])) for byte in data))
                wait_file(root / 'disk.paused')
            else:
                first.sendall(begin(request_id, BMP) + frame(0x22, request_id, BMP))
            wait_file(root / 'disk.blocked')

            if expire:
                nonce = receive_frame(first, answer_pings=False)
                assert nonce[0] == 0x30 and nonce[1] == 0
                first.sendall(frame(0x31, 0, nonce[2]))
                deadline = time.monotonic() + 5
                while not select.select([first], [], [], .02)[0]:
                    assert time.monotonic() < deadline
                    answer_heartbeats([second])
                try:
                    assert first.recv(1) == b''
                except ConnectionResetError:
                    pass
                server.expect(b'[client 3] frame timeout')
                server.expect(b'[client 3] disconnected')
                server.expect(b'[client 1] heartbeat timeout')
                server.expect(f'[client 1 request {request_id}] interrupted'.encode())
                server.expect(b'[client 1] disconnected')
                (root / 'disk.release').touch()
                for _ in range(2):
                    server.expect(b'screenshot canceled')
                for client_id in [1, 3]:
                    assert f'[client {client_id} request 1] screenshot canceled'.encode() in server.output
                partial.close()
                server.command(b':use 2')
                server.expect(b'Selected client 2')
                server.command(b'after-paused-timeout')
                other = server.receive(second, b'after-paused-timeout')
                second.sendall(output(other, b'healthy-after-timeout\n') + complete(other))
                server.expect(b'healthy-after-timeout\n')
                assert len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == baseline_fds + 1
                print('PASS: frame/paused-heartbeat expiry, deferred cleanup, healthy peer and FD recovery')
            else:
                other = screenshot(server, second, 2)
                second.sendall(begin(other, BMP) + frame(0x22, other, BMP) + complete(other) +
                               frame(0x30, 0, b'queue-ok'))
                assert receive_frame(second) == (0x31, 0, b'queue-ok')
                assert not list(root.rglob('*.bmp'))
                server.command(b'quit')
                assert second.recv(1) == b''
                assert server.process.poll() is None
                assert list(root.rglob('*.part'))
                (root / 'disk.release').touch()
                server.process.wait(timeout=5)
                assert server.process.returncode == 0
                print('PASS: shutdown waits for active write, cancels queued commit and removes partial files')
        finally:
            (root / 'disk.release').touch()
            server.close()
        assert not list(root.rglob('*.part'))
        assert not list(root.rglob('*.bmp'))


lifecycle(expire='--fast-clock' in sys.argv[2:])
