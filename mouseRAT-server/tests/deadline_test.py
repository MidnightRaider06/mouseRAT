import struct
import time
from pathlib import Path

from multiclient_test import Server, complete, frame, output, receive_frame


server = Server()
try:
    server.expect(b'Controls:')
    baseline_fds = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
    healthy = server.connect(1)
    server.connect(2, handshake=False)
    partial_hello = server.connect(3, handshake=False)
    partial_hello.sendall(frame(0x01)[:1])

    ping = frame(0x30, 0, struct.pack('!Q', 123))
    header = server.connect(4)
    header.sendall(ping[:1])
    payload = server.connect(5)
    server.command(b':use 5')
    server.expect(b'Selected client 5')
    server.command(b'pending')
    request_id = server.receive(payload, b'pending')
    payload.sendall(output(request_id, b'x')[:-1])
    trickle = server.connect(6)
    trickle.sendall(ping[:1])
    reset = server.connect(7)
    reset.sendall(ping[:1])
    late_hello = server.connect(8, handshake=False)
    late_hello.sendall(frame(0x01)[:7])

    started = time.monotonic()

    def until(seconds):
        time.sleep(max(0, started + seconds - time.monotonic()))

    def healthy_command(text, expected_id):
        server.command(b':use 1')
        server.expect(b'Selected client 1')
        server.command(text)
        assert server.receive(healthy, text) == expected_id
        healthy.sendall(complete(expected_id))
        server.expect(f'[client 1 request {expected_id}] complete (exit 0)'.encode())

    until(2)
    late_hello.sendall(frame(0x01)[7:])
    assert receive_frame(late_hello) == (0x02, 0, b'')
    server.expect(b'[client 8] ready')
    until(6)
    for client_id in [2, 3]:
        server.expect(f'[client {client_id}] handshake timeout'.encode())
        server.expect(f'[client {client_id}] disconnected'.encode())
    healthy_command(b'during-partial-frames', 1)

    until(9)
    trickle.sendall(ping[1:2])
    reset.sendall(ping[1:] + ping[:1])
    assert receive_frame(reset) == (0x31, 0, struct.pack('!Q', 123))
    until(20)
    trickle.sendall(ping[2:3])
    until(31)
    for client_id in [4, 5, 6]:
        server.expect(f'[client {client_id}] frame timeout'.encode())
        if client_id == 5:
            server.expect(f'[client 5 request {request_id}] interrupted'.encode())
        server.expect(f'[client {client_id}] disconnected'.encode())

    reset.sendall(ping[1:])
    assert receive_frame(reset) == (0x31, 0, struct.pack('!Q', 123))
    late_hello.sendall(ping)
    assert receive_frame(late_hello) == (0x31, 0, struct.pack('!Q', 123))
    healthy_command(b'after-timeouts', 2)
    assert len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == baseline_fds + 3
    print('PASS: handshake/frame deadlines, trickled bytes, timer reset, isolation and cleanup')
finally:
    server.close()
