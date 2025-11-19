import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import time

SERVER = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'build' / 'server'
HEADER = struct.Struct('!4sBBHII')


def frame(kind, request_id=0, payload=b''):
    return HEADER.pack(b'MRAT', 1, kind, 0, len(payload), request_id) + payload


def receive_frame(peer, answer_pings=True):
    def exact(size):
        data = b''
        while len(data) < size:
            chunk = peer.recv(size - len(data))
            assert chunk, data
            data += chunk
        return data

    while True:
        magic, version, kind, flags, size, request_id = HEADER.unpack(exact(HEADER.size))
        assert magic == b'MRAT' and version == 1 and flags == 0 and size <= 65536
        payload = exact(size)
        if kind == 0x30 and answer_pings:
            assert request_id == 0 and len(payload) == 8
            peer.sendall(frame(0x31, 0, payload))
        else:
            return kind, request_id, payload


def answer_heartbeats(peers):
    for peer in peers:
        if select.select([peer], [], [], 0)[0]:
            kind, request_id, nonce = receive_frame(peer, answer_pings=False)
            assert kind == 0x30 and request_id == 0 and len(nonce) == 8
            peer.sendall(frame(0x31, 0, nonce))


def output(request_id, data):
    return frame(0x11, request_id, b'\0' + data)


def complete(request_id, status=0):
    return frame(0x12, request_id, struct.pack('!I', status))


class Server:
    def __init__(self, cwd=None, env=None):
        self.process = subprocess.Popen([str(SERVER)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
                                        cwd=cwd, env=env)
        self.output = b''
        self.cursor = 0
        self.peers = []

    def expect(self, text):
        deadline = time.monotonic() + 5
        while text not in self.output[self.cursor:]:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (text, self.output[-2000:])
            ready, _, _ = select.select([self.process.stdout], [], [], remaining)
            assert ready, (text, self.output[-2000:])
            chunk = os.read(self.process.stdout.fileno(), 65536)
            assert chunk, (self.process.poll(), text, self.output[-2000:])
            self.output += chunk
        self.cursor = self.output.index(text, self.cursor) + len(text)

    def command(self, text):
        self.process.stdin.write(text + b'\n')

    def connect(self, client_id, handshake=True):
        peer = socket.create_connection(('127.0.0.1', 5555), timeout=3)
        self.peers.append(peer)
        self.expect(f'[client {client_id}] connected'.encode())
        if handshake:
            peer.sendall(frame(0x01))
            assert receive_frame(peer) == (0x02, 0, b'')
            self.expect(f'[client {client_id}] ready'.encode())
        return peer

    def receive(self, peer, command):
        kind, request_id, payload = receive_frame(peer)
        assert kind == 0x10 and request_id != 0 and payload == command
        return request_id

    def close(self):
        try:
            if self.process.poll() is None:
                self.command(b'quit')
            self.process.communicate(timeout=5)
            assert self.process.returncode == 0, self.output[-2000:]
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.communicate()
            for peer in self.peers:
                peer.close()


def multiplexing():
    server = Server()
    try:
        server.expect(b'Controls:')
        baseline_fds = len(list(Path(f'/proc/{server.process.pid}/fd').iterdir()))
        first = server.connect(1)
        second = server.connect(2)
        server.command(b':clients')
        server.expect(b'[client 1] 127.0.0.1:')
        server.expect(b'[client 2] 127.0.0.1:')

        server.command(b':use 1')
        server.expect(b'Selected client 1')
        server.command(b'alpha')
        assert server.receive(first, b'alpha') == 1
        assert not select.select([second], [], [], .05)[0]
        server.command(b'extra')
        server.expect(b'still has a pending command')

        server.process.stdin.write(b':us')
        server.command(b'e 2')
        server.expect(b'Selected client 2')
        server.command(b'beta')
        assert server.receive(second, b'beta') == 1
        nonce = struct.pack('!Q', 12345)
        second.sendall(frame(0x30, 0, nonce))
        assert receive_frame(second) == (0x31, 0, nonce)
        second.sendall(output(1, b'B\x00response\n') + complete(1))
        server.expect(b'B\x00response\n')
        server.expect(b'[client 2 request 1] complete (exit 0)')

        split = output(1, b'first-fragment-')
        first.sendall(split[:18])
        server.command(b':use 1')
        server.expect(b'Selected client 1')
        server.command(b'extra')
        server.expect(b'still has a pending command')
        first.sendall(split[18:])
        server.expect(b'first-fragment-')
        server.command(b'extra')
        server.expect(b'still has a pending command')
        first.sendall(output(1, b'second-fragment\n') + complete(1, 7))
        server.expect(b'second-fragment\n')
        server.expect(b'[client 1 request 1] complete (exit 7)')

        server.command(b':use 1')
        server.expect(b'Selected client 1')
        server.command(b'reset-me')
        assert server.receive(first, b'reset-me') == 2
        first.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        first.close()
        server.expect(b'[client 1 request 2] interrupted')
        server.expect(b'[client 1] disconnected')
        server.command(b':use 1')
        server.expect(b'Unknown client ID')
        server.command(b'no-target')
        server.expect(b'No client selected')

        third = server.connect(3, handshake=False)
        server.command(b':use 3')
        server.expect(b'Selected client 3')
        server.command(b'not-ready')
        server.expect(b'Selected client is awaiting HELLO')
        hello = frame(0x01)
        third.sendall(hello[:15])
        server.command(b'not-ready')
        server.expect(b'Selected client is awaiting HELLO')
        third.sendall(hello[15:])
        assert receive_frame(third) == (0x02, 0, b'')
        server.expect(b'[client 3] ready')

        server.command(b'x' * 8192)
        server.expect(b'Input line too long')
        server.command(b'x' * 4096)
        assert server.receive(third, b'x' * 4096) == 1
        third.sendall(complete(1))
        server.expect(b'[client 3 request 1] complete (exit 0)')
        server.command(b'bad\x00command')
        server.expect(b'Command must be 1 to 4096 bytes without NUL')
        server.command(b'bad\xed\xa0\x80')
        server.expect(b'Command must be 1 to 4096 bytes without NUL and valid UTF-8')
        server.command(b'echo \xe2\x82\xac')
        assert server.receive(third, b'echo \xe2\x82\xac') == 2
        third.sendall(output(2, b'opaque\xff\x00\n') + complete(2))
        server.expect(b'opaque\xff\x00\n')
        server.expect(b'[client 3 request 2] complete (exit 0)')
        server.command(b'screenshot')
        assert receive_frame(third) == (0x20, 3, b'')
        third.sendall(frame(0x7f, 3, struct.pack('!I', 1) + b'unsupported capture'))
        server.expect(b'[client 3 request 3] error 1: unsupported capture')
        third.close()
        server.expect(b'[client 3] disconnected')

        server.command(b':use 2')
        server.expect(b'Selected client 2')
        server.command(b'delta')
        assert server.receive(second, b'delta') == 2
        second.sendall(output(2, b'Z' * 8192 + b'TAIL\n') + complete(2))
        second.shutdown(socket.SHUT_WR)
        server.expect(b'TAIL\n')
        server.expect(b'[client 2 request 2] complete (exit 0)')
        server.expect(b'[client 2] disconnected')
        assert server.output.count(b'Z') == 8192
        assert len(list(Path(f'/proc/{server.process.pid}/fd').iterdir())) == baseline_fds
        print('PASS: handshake, framing, terminal completion, switching, reset, limits and cleanup')
    finally:
        server.close()


def capacity():
    server = Server()
    try:
        server.expect(b'Controls:')
        for client_id in range(1, 65):
            server.connect(client_id)
        overflow = socket.create_connection(('127.0.0.1', 5555), timeout=3)
        server.peers.append(overflow)
        server.expect(b'Client limit reached')
        assert overflow.recv(1) == b''
        server.command(b':use 64')
        server.expect(b'Selected client 64')
        server.command(b'capacity-check')
        request_id = server.receive(server.peers[63], b'capacity-check')
        server.peers[63].sendall(output(request_id, b'capacity-response\n') + complete(request_id))
        server.expect(b'capacity-response\n')
        print('PASS: 64 clients remain usable and connection 65 is rejected')
    finally:
        server.close()


def protocol_errors():
    server = Server()
    try:
        server.expect(b'Controls:')
        healthy = server.connect(1)
        packets = [
            b'legacy text data',
            frame(0x02),
            frame(0x11, 1, b'\0x'),
            HEADER.pack(b'MRAT', 2, 0x01, 0, 0, 0),
            HEADER.pack(b'MRAT', 1, 0x01, 1, 0, 0),
            HEADER.pack(b'MRAT', 1, 0x11, 0, 65537, 1),
        ]
        for client_id, packet in enumerate(packets, 2):
            peer = server.connect(client_id, handshake=False)
            peer.sendall(packet)
            server.expect(f'[client {client_id}] protocol violation'.encode())
            server.expect(f'[client {client_id}] disconnected'.encode())

        repeated = server.connect(8)
        repeated.sendall(frame(0x01))
        server.expect(b'[client 8] protocol violation')
        server.expect(b'[client 8] disconnected')

        wrong_id = server.connect(9)
        server.command(b':use 9')
        server.expect(b'Selected client 9')
        server.command(b'pending')
        request_id = server.receive(wrong_id, b'pending')
        wrong_id.sendall(complete(request_id + 1))
        server.expect(b'[client 9] protocol violation')
        server.expect(b'[client 9] disconnected')

        truncated = server.connect(10)
        truncated.sendall(output(1, b'incomplete')[:8])
        truncated.shutdown(socket.SHUT_WR)
        server.expect(b'[client 10] truncated frame')
        server.expect(b'[client 10] disconnected')

        for client_id, payload in enumerate([struct.pack('!I', 5),
                                              struct.pack('!I', 2) + b'bad\xc0\x80'], 11):
            peer = server.connect(client_id)
            server.command(f':use {client_id}'.encode())
            server.expect(f'Selected client {client_id}'.encode())
            server.command(b'pending')
            request_id = server.receive(peer, b'pending')
            peer.sendall(frame(0x7f, request_id, payload))
            server.expect(f'[client {client_id}] protocol violation'.encode())
            server.expect(f'[client {client_id} request {request_id}] interrupted'.encode())
            server.expect(f'[client {client_id}] disconnected'.encode())

        server.command(b':use 1')
        server.expect(b'Selected client 1')
        server.command(b'failed-command')
        request_id = server.receive(healthy, b'failed-command')
        healthy.sendall(frame(0x7f, request_id, struct.pack('!I', 2) + b'mock failure'))
        server.expect(b'error 2: mock failure')
        server.command(b'still-healthy')
        request_id = server.receive(healthy, b'still-healthy')
        healthy.sendall(complete(request_id))
        server.expect(b'[client 1 request 2] complete (exit 0)')
        print('PASS: malformed frames, payload validation, wrong IDs, truncation and error isolation')
    finally:
        server.close()


if __name__ == '__main__':
    multiplexing()
    capacity()
    protocol_errors()
