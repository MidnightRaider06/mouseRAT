import struct
import sys
import time
from pathlib import Path

from multiclient_test import Server, answer_heartbeats, complete, frame, output, receive_frame


assert len(sys.argv) == 2, 'Pass the server linked with fast_clock.cpp and its GNU linker wrapper'
server = Server()
try:
    server.expect(b'Controls:')
    baseline_fds = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
    peers = {client_id: server.connect(client_id) for client_id in range(1, 7)}

    def select(client_id):
        server.command(f':use {client_id}'.encode())
        server.expect(f'Selected client {client_id}'.encode())

    def queue(client_id, request_id):
        select(client_id)
        server.command(b'pending')
        assert server.receive(peers[client_id], b'pending') == request_id

    def finished(client_id, request_id):
        peers[client_id].sendall(complete(request_id))
        server.expect(f'[client {client_id} request {request_id}] complete (exit 0)'.encode())

    select(5)
    server.command(b'bad\0command')
    server.expect(b'Command must be 1 to 4096 bytes without NUL')
    for client_id in [1, 2, 3, 4, 6]:
        queue(client_id, 1)

    started = time.monotonic()
    for second in range(1, 29):
        time.sleep(max(0, started + second - time.monotonic()))
        answer_heartbeats(peers.values())
        nonce = struct.pack('!Q', second)
        peers[2].sendall(output(1, b'ongoing\n') + frame(0x30, 0, nonce))
        assert receive_frame(peers[2]) == (0x31, 0, nonce)

        if second == 8:
            finished(3, 1)
            peers[4].sendall(frame(0x7f, 1, struct.pack('!I', 2)))
            server.expect(b'[client 4 request 1] error 2:')
            finished(6, 1)
        if second == 15:
            select(2)
            server.command(b'busy')
            server.expect(b'Selected client still has a pending command')
        if second == 18:
            queue(6, 2)

    while time.monotonic() < started + 31:
        answer_heartbeats([peers[client_id] for client_id in [3, 4, 5, 6]])
        time.sleep(.1)
    for client_id in [1, 2]:
        server.expect(f'[client {client_id}] request timeout'.encode())
        server.expect(f'[client {client_id} request 1] interrupted'.encode())
        server.expect(f'[client {client_id}] disconnected'.encode())

    for client_id, request_id in [(3, 2), (4, 2), (5, 1)]:
        queue(client_id, request_id)
        finished(client_id, request_id)
    finished(6, 2)
    assert len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == baseline_fds + 4
    print('PASS: absolute request deadlines, ongoing traffic, admission, completion, isolation and cleanup')
finally:
    server.close()
