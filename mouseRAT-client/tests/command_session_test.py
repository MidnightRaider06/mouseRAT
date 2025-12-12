"""Exercise real Windows commands through a local framed mock server."""

import ctypes
from pathlib import Path
import socket
import struct
import sys
import time

from screenshot_session_test import connect, frame, receive, stop


def command(peer, request_id, text, result=0, error=None, slow=False, idle_ping=False):
    request = frame(0x10, request_id, text.encode('utf-8'))
    nonce = struct.pack('!Q', request_id)
    peer.sendall(request[:7])
    peer.sendall(request[7:] + frame(0x30, payload=nonce))
    if slow:
        time.sleep(1)
    output = bytearray()
    pong = ping = False
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        kind, response_id, payload = receive(peer)
        if kind == 0x30:
            assert response_id == 0 and len(payload) == 8
            peer.sendall(frame(0x31, payload=payload))
            ping = True
        elif kind == 0x31:
            assert response_id == 0 and payload == nonce and not pong
            pong = True
        elif kind == 0x11:
            assert response_id == request_id and 2 <= len(payload) <= 65536 and payload[0] == 0
            output.extend(payload[1:])
            assert len(output) <= 16 * 1024 * 1024
        else:
            assert response_id == request_id and pong and (ping or not idle_ping)
            if error is None:
                assert (kind, payload) == (0x12, struct.pack('!I', result)), (kind, payload)
            else:
                assert kind == 0x7f and struct.unpack('!I', payload[:4])[0] == error, (kind, payload)
                payload[4:].decode('utf-8', errors='strict')
                assert peer.recv(1) == b'', 'Failed execution left its session open'
            return bytes(output)
    raise AssertionError('Command response deadline exceeded')


def main():
    executable = str(Path(sys.argv[1]).resolve(strict=True))
    fixture = f'"{sys.executable}" -B "{Path(__file__).resolve()}"'
    binary = bytes(range(256)) * 1024
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 5555))
        listener.listen(2)
        listener.settimeout(5)
        process, peer = connect(listener, executable)
        peer.settimeout(15)
        try:
            assert command(peer, 1, 'rem no output') == b''
            assert command(peer, 2, 'echo hello') == b'hello\r\n'
            assert command(peer, 3, 'exit /b -1', result=0xffffffff) == b''
            command(peer, 4, 'set "MRAT_STATE=\u03bb\u4e2d\U0001f600"\r\ncd /d "%SystemRoot%"')
            state = command(peer, 5, 'echo %MRAT_STATE%\r\n'
                            'if /i "%CD%"=="%SystemRoot%" (echo cwd_ok) else (exit /b 9)\r\n'
                            'for %%x in (one two) do @echo %%x')
            assert state == '\u03bb\u4e2d\U0001f600\r\ncwd_ok\r\none\r\ntwo\r\n'.encode()
            assert command(peer, 6, fixture + ' --binary', result=37) == binary
            assert command(peer, 7, f'for /l %%x in (1,1,8) do @{fixture} --binary\r\nexit /b 37',
                           result=37, slow=True) == binary * 8
            assert command(peer, 8, 'ping -n 13 127.0.0.1 >nul\r\nexit /b 7',
                           result=7, idle_ping=True) == b''
        finally:
            stop(process, peer, 0)
        print('PASS: framed commands, empty/nonzero results, Unicode state, binary output, slow reads and heartbeats')

        for text, code in [('set "ERRORLEVEL=123"', 2), (fixture + ' --flood', 4)]:
            process, peer = connect(listener, executable)
            peer.settimeout(15)
            try:
                data = command(peer, 1, text, error=code)
                if code == 4:
                    assert 0 < len(data) <= 16 * 1024 * 1024
                else:
                    assert not data
            finally:
                stop(process, peer, 0)
        print('PASS: unknown status and output limit stop execution, send ERROR and close the session')

        process, peer = connect(listener, executable)
        try:
            peer.sendall(frame(0x10, 1, b'ping -n 20 127.0.0.1 >nul') + frame(0x10, 2, b'echo overlap'))
            process.wait(timeout=5)
        finally:
            stop(process, peer, 1)
        print('PASS: overlapping command requests close the session')

        process, peer = connect(listener, executable)
        child = None
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        try:
            peer.sendall(frame(0x10, 1, (fixture + ' --hold').encode('utf-8')))
            output = bytearray()
            while b'\n' not in output:
                kind, request_id, payload = receive(peer)
                assert (kind, request_id, payload[:1]) == (0x11, 1, b'\0')
                output.extend(payload[1:])
            child = kernel.OpenProcess(0x100000, False, int(output.strip()))
            assert child, ctypes.get_last_error()
        finally:
            try:
                stop(process, peer, 1)
                if child:
                    assert kernel.WaitForSingleObject(child, 5000) == 0, 'Disconnect left the command running'
            finally:
                if child:
                    kernel.CloseHandle(child)
        print('PASS: disconnect terminates the session-owned command process')


if __name__ == '__main__':
    if sys.argv[1:] == ['--hold']:
        import os
        print(os.getpid(), flush=True)
        time.sleep(60)
    elif sys.argv[1:] in (['--binary'], ['--flood']):
        for _ in range(80 if sys.argv[1] == '--flood' else 1):
            sys.stdout.buffer.write(bytes(range(256)) * 1024)
        sys.stdout.buffer.flush()
        sys.exit(37)
    else:
        main()
