# Binary protocol v1

Status: implementation contract for upcoming server and Windows client changes.
The running server still uses the legacy text protocol. Neither endpoint implements
this document yet. Switching requires updating both endpoints, with no text fallback.

## Frame header

Every frame is a 16-byte header followed by exactly `payload_length` payload bytes.
All multibyte integers are unsigned and encoded in big-endian order. Offsets are bytes.
Encode fields individually rather than copying a native C++ struct onto the wire.

| Offset | Width | Field | Required value or meaning |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `MRAT`, hex `4d 52 41 54` |
| 4 | 1 | version | `1` |
| 5 | 1 | type | Message type below |
| 6 | 2 | flags | `0` in v1 |
| 8 | 4 | payload_length | Payload bytes only, at most 65,536 |
| 12 | 4 | request_id | Nonzero for requests and their responses, otherwise zero |

TCP read boundaries have no meaning to the protocol. A decoder accumulates the
header, validates it, and then accumulates exactly the declared payload. It retains
incomplete frames and processes additional frames if a read contains several.
Partial writes retain their offset until all header and payload bytes are sent.

## Message types

S is the Linux server. C is the Windows client. Every payload integer follows the
header's byte order. All payload lengths below are in bytes.

| Type | Name | Direction | Payload |
| --- | --- | --- | --- |
| `0x01` | HELLO | C to S | Empty, request ID zero |
| `0x02` | HELLO_ACK | S to C | Empty, request ID zero |
| `0x10` | COMMAND | S to C | 1 to 4096 bytes of UTF-8 command text, no NUL |
| `0x11` | OUTPUT | C to S | One stream byte followed by 1 to 65,535 output bytes |
| `0x12` | COMPLETE | C to S | One `uint32` result |
| `0x20` | SCREENSHOT | S to C | Empty |
| `0x21` | IMAGE_BEGIN | C to S | `uint64` total image bytes, then `uint32` image format |
| `0x22` | IMAGE_DATA | C to S | 1 to 65,536 image bytes |
| `0x30` | PING | Either | One `uint64` nonce, request ID zero |
| `0x31` | PONG | Either | Exact nonce bytes from PING, request ID zero |
| `0x7f` | ERROR | C to S | `uint32` error code followed by 0 to 1024 UTF-8 diagnostic bytes |

OUTPUT stream values are `0` for combined stdout/stderr, `1` for stdout, and `2`
for stderr. Output bytes are opaque, may contain NUL, and need not be UTF-8. The
client sends the shell's output without echo markers or server-added suffixes.
COMPLETE contains the shell exit code for COMMAND, including nonzero exit codes.

Image format values are `1` for PNG and `2` for BMP. The total counts the entire
encoded file including its headers. IMAGE_DATA carries raw file bytes, not base64.
For screenshots COMPLETE must contain zero. ERROR codes are `1` unsupported request,
`2` execution failure, `3` capture failure, and `4` resource limit exceeded.
Diagnostic text is for display, never for machine decisions.

## Session and request state

1. The client sends HELLO first. The server replies HELLO_ACK. Both messages are
   mandatory, and the handshake must finish within five seconds of connection.
2. After the handshake the server can issue one active request per client. It
   allocates IDs starting at one, never reusing an ID within a connection. After
   ID `0xffffffff`, reconnect before issuing another request. These IDs are separate
   from the server's local connection IDs.
3. COMMAND permits zero or more OUTPUT frames followed by exactly one COMPLETE
   or ERROR. A command with no output still sends COMPLETE. Request completion is
   determined by the terminal frame, never by a data frame or a quiet socket.
4. SCREENSHOT permits exactly one IMAGE_BEGIN, then IMAGE_DATA frames, then COMPLETE.
   The data lengths must sum to the declared total. ERROR can terminate at any stage.
5. Every response repeats the active request ID. No response data is allowed after
   COMPLETE or ERROR. OUTPUT is invalid for a screenshot, and image frames are
   invalid for a command. Only a terminal frame frees the pending-request slot.
6. PING and PONG can appear between frames after the handshake, including during
   an active request. Keep at most one unanswered PING per direction. Send one
   after ten seconds without receiving a frame, and close if its matching PONG
   has not arrived within thirty seconds of sending it.

## Limits, backpressure, and failure

- The v1 limits are 16 MiB of output data per command, excluding stream bytes, and
  32 MiB per screenshot. Declared image size must be nonzero and within the limit.
  A client unable to stay within a request limit stops that request and sends ERROR.
  If it cannot stop the request's producer safely, it closes the session instead.
  The server independently counts received bytes and closes a violating connection.
- Planned server defaults are 64 clients, one receive frame buffer per connection,
  at most 128 KiB of queued outbound bytes per connection, and an 8 MiB global image
  disk-write queue. These bounds exclude kernel socket buffers and map overhead.
- Producers pause when a bounded queue fills. They retain unsent bytes and resume
  when capacity returns. No frame bytes are dropped, and no unbounded retry queue
  is created. A new request is rejected locally while another is active. When the
  disk queue fills, pause reads for affected connections rather than accumulating
  whole images in memory. TCP flow control then slows their senders.
- Allow at most thirty seconds from a frame's first byte to its final byte and
  five minutes from queuing a request to receiving its terminal response. These are
  absolute deadlines, including local backpressure. PONG does not extend them.
  A timeout closes that connection and fails its active request.
- Bad magic, version, flags, type, direction, payload shape, request ID, or state
  closes the connection. Validate header limits before accepting payload storage.
  Do not attempt to resynchronize by searching binary data for the magic bytes.
- EOF or socket failure during a request fails it. Discard partial frames and image
  files. A disconnect after sending a command leaves its execution outcome unknown.
  Do not retry automatically because execution may already have had side effects.
  On session closure the client stops session-owned producers and releases handles.
- Generate image filenames on the server, with no remote-supplied paths. Stream to
  a temporary file and publish it only after COMPLETE, exact byte-count checks,
  expected image-signature validation, and successful file writes. ERROR, storage
  failure, or disconnect removes the temporary file. Storage failure closes the
  affected connection. COMPLETE alone does not establish local persistence.

Framing provides neither authentication nor encryption. A future TLS layer can
carry the same frame bytes. No custom encryption is defined here.

## Examples

COMMAND `dir` with request ID one, shown as header then payload:

```text
4d 52 41 54 01 10 00 00 00 00 00 03 00 00 00 01 | 64 69 72
```

A zero-exit-code COMPLETE for that command:

```text
4d 52 41 54 01 12 00 00 00 00 00 04 00 00 00 01 | 00 00 00 00
```

Example exchanges, with arrows indicating direction:

```text
C -> S  HELLO(0)
S -> C  HELLO_ACK(0)
S -> C  COMMAND(1, "dir")
C -> S  OUTPUT(1, stdout, part A)
C -> S  OUTPUT(1, stdout, part B)
C -> S  COMPLETE(1, exit_code=0)
S -> C  SCREENSHOT(2)
C -> S  IMAGE_BEGIN(2, total=100000, format=PNG)
C -> S  IMAGE_DATA(2, 65536 bytes)
C -> S  IMAGE_DATA(2, 34464 bytes)
C -> S  COMPLETE(2, result=0)
```
