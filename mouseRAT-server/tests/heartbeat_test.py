import select
import sys
import time
from pathlib import Path

from multiclient_test import Server, answer_heartbeats, complete, frame, output, receive_frame


assert len(sys.argv) == 2, 'Pass the server linked with the accelerated clock fixture'
server = Server()
try:
    server.expect(b'Controls:')
    baseline_fds = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
    healthy = server.connect(1)
    ignoring = server.connect(2)
    wrong = server.connect(3)
    unsolicited = server.connect(4)
    unsolicited.sendall(frame(0x31, 0, b'12345678'))
    server.expect(b'[client 4] protocol violation')
    server.expect(b'[client 4] disconnected')
    duplicate = server.connect(5)
    idle = server.connect(6)

    for client_id, peer in [(1, healthy), (2, ignoring)]:
        server.command(f':use {client_id}'.encode())
        server.expect(f'Selected client {client_id}'.encode())
        server.command(b'pending')
        assert server.receive(peer, b'pending') == 1

    kind, request_id, nonce = receive_frame(healthy, answer_pings=False)
    assert kind == 0x30 and request_id == 0 and len(nonce) == 8
    pong = frame(0x31, 0, nonce)
    healthy.sendall(pong[:7])
    healthy.sendall(pong[7:] + output(1, b'heartbeat-output\n'))
    server.expect(b'heartbeat-output\n')
    server.command(b':use 1')
    server.expect(b'Selected client 1')
    server.command(b'extra')
    server.expect(b'still has a pending command')
    healthy.sendall(complete(1))
    server.expect(b'[client 1 request 1] complete (exit 0)')

    assert receive_frame(ignoring, answer_pings=False)[0] == 0x30
    received_ping = time.monotonic()
    kind, request_id, bad_nonce = receive_frame(wrong, answer_pings=False)
    assert kind == 0x30 and request_id == 0
    wrong.sendall(frame(0x31, 0, bytes([bad_nonce[0] ^ 1]) + bad_nonce[1:]))
    server.expect(b'[client 3] protocol violation')
    server.expect(b'[client 3] disconnected')

    kind, request_id, duplicate_nonce = receive_frame(duplicate, answer_pings=False)
    assert kind == 0x30 and request_id == 0
    duplicate.sendall(frame(0x31, 0, duplicate_nonce) * 2)
    server.expect(b'[client 5] protocol violation')
    server.expect(b'[client 5] disconnected')
    assert receive_frame(idle, answer_pings=False)[0] == 0x30

    kind, request_id, next_nonce = receive_frame(healthy, answer_pings=False)
    assert kind == 0x30 and request_id == 0 and next_nonce != nonce
    healthy.sendall(frame(0x31, 0, next_nonce))
    ignoring.sendall(frame(0x30, 0, b'peerping'))
    assert receive_frame(ignoring, answer_pings=False) == (0x31, 0, b'peerping')
    while time.monotonic() < received_ping + 2.5:
        ignoring.sendall(output(1, b'ongoing\n'))
        answer_heartbeats([healthy])
        time.sleep(.2)
    assert not select.select([ignoring], [], [], 0)[0]

    server.expect(b'[client 2] heartbeat timeout')
    server.expect(b'[client 2 request 1] interrupted')
    server.expect(b'[client 2] disconnected')
    server.expect(b'[client 6] heartbeat timeout')
    server.expect(b'[client 6] disconnected')
    server.command(b':use 1')
    server.expect(b'Selected client 1')
    server.command(b'still-healthy')
    assert server.receive(healthy, b'still-healthy') == 2
    healthy.sendall(complete(2))
    server.expect(b'[client 1 request 2] complete (exit 0)')
    assert len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == baseline_fds + 1
    print('PASS: heartbeat scheduling, PONG validation, traffic-independent expiry, isolation and cleanup')
finally:
    server.close()
