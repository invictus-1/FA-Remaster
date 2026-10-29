# The UDP connector: the original's wire format and behaviour, and ours

`ui/net/connector_udp.cpp` is built from this read of the original (CNetUDPConnector / CNetUDPConnection). The read
itself follows unchanged below the line. Where ours differs (2026-10-29):

- **Our sends are not deflated** (`net_CompressionMethod` 0, a setting the original has; its default is 1): the
  repository's zlib holds only inflate's files. A peer's deflated stream is inflated as the original does, so an
  original peer at its default still talks with ours. `MOHO64_NET_DEFLATE=1` (with zlib's deflate.c / trees.c added)
  gives the original's 1.
- **No NAT traversal provider**: datagrams of type 8 are dropped.
- **The thread waits on `select`** and is woken by a one-byte datagram to its own port (ignored as a short packet);
  the original waits on a WSA event.
- **Connect replaces a TimedOut connection** to the same address (the original asserts there, line 0x95b).
- **A destroyed connector's thread lingers at most 3 s** to see its end-of-stream and goodbyes out (the original's
  lifetime there was not read).
- **The lobby side** (ui/net/lobby_core.cpp): both ends of a joiner pair connect (UDP), an incoming connection is
  accepted at the host and, at a joiner, when it comes from a peer the host named (an unknown one is left waiting,
  INFERRED). A joiner whose host never answers keeps trying, as the original's connector does (its lobby's give-up
  was not read).
- Not wire-compatible with FAF's patched exe when FAF's marker feature is on (section 9).

---

# UDP connector: wire format and behaviour (CNetUDPConnector / CNetUDPConnection)

Read from the pinned `bin\ForgedAlliance.exe` (MSVC 8, 32-bit) on 2026-10-29 with `uidis.py`. Source file name in
the asserts: `c:\work\rts\main\code\src\core\NetUDPConn.cpp`. Every claim is tagged **READ @addr** (seen in the
code at that address) or **INFERRED** (with the reason). Nothing here comes from library source.

**FAF patch warning.** This exe is FAF's patched build: `SendPacket` (0x488d80), `ReceiveData` (0x48a280) and the
connector thread (0x489f90) start with a `jmp` into the `.exxt` section (0x128ecf0, 0x128eb30, 0x128f100). The
patched copies are the original code plus one dormant FAF feature (see section 9). With FAF's flags at their
default 0 the bytes on the wire are the original's. Everything below is the original behaviour unless marked FAF.

Conventions: "conn+X" = field of CNetUDPConnection, "ctr+X" = field of CNetUDPConnector, "pkt+X" = field of the
SNetPacket buffer object (the datagram starts at pkt+0x18, so header offset h = pkt+0x18+h).

---------------------------------------------------------------------------------------------------------------

## 1. Header (15 bytes, every packet except type 8)

All fields are **little-endian** (plain x86 stores, no htons/htonl on them; only the sockaddr uses htons/htonl):
READ @0x488b20 (NewPacket), @0x488d87.. (SendPacket), @0x488980 (ReadPacket).

| Off | Size | Name | Sender fills | Receiver checks |
|---|---|---|---|---|
| 0 | u8 | type | 0..7 (table in section 2) | 0..8 valid; >= 9 "ignoring unknown packet type" READ @0x48a6af |
| 1 | u32 | **ack mask** (selective ack) | conn+0xd94: bit i set = DATA seq `expected+1+i` is held in the early-packet list (i = 0..31). 0 in CONNECT/ANSWER. READ @0x488b9a, 0x4872b7 | for each unacked DATA p: `d = (i16)(p.seq - hdr.expected)`; d in 1..32 and bit (d-1) set -> p is acked. READ @0x486cb1 |
| 5 | u16 | **serial number** | conn+0x4a0, written by SendPacket into EVERY datagram it sends (resends too), then +1. Starts at 1. READ @0x488da1, 0x48901b, ctor 0x485e62 | used as the "in response to" value of our next packet (AdoptPacket) |
| 7 | u16 | **in response to** | conn+0x4a2 = serial of the last packet we "adopted" (CONNECT, ANSWER, DATA, KEEPALIVE) since our last send; 0 = none. Written by SendPacket, then reset to 0. READ @0x488db3, 0x48902b, AdoptPacket 0x488220 | ping sample if != 0 and our send-history slot `[irt % 128]` holds that serial (section 5) |
| 9 | u16 | **sequence number** | DATA: its own seq (conn+0x4b0, then conn+0x4b0 += 1). ACK/KEEPALIVE/GOODBYE: the next unused seq (not consumed). CONNECT/ANSWER: 0. READ @0x4889ef, 0x488a79, 0x488aee, 0x488b74 | only DATA uses it (section 4) |
| 11 | u16 | **expected sequence number** (cumulative ack) | conn+0x4b4 = next DATA seq we want from the peer (all below it delivered). CONNECT/ANSWER: 0. READ @0x488b87, 0x4889fa | `(i16)(ourNextSeq - hdr.expected) < 0` -> "acknowledgement of data we haven't sent", packet dropped. Else every unacked DATA with `(i16)(p.seq - hdr.expected) < 0` is acked; conn+0x4b2 (remote expected) = max(itself, hdr.expected). READ @0x486c1b..0x486d03 |
| 13 | u16 | payload length | datagram size - 15 | must equal recvfrom size - 15, else "payload length mismatch" READ @0x48a708 |

Note on resends: the unacked DATA buffer is resent as-is except serial (5) and in-response-to (7), which SendPacket
rewrites. Mask (1) and expected (11) keep the values from when the packet was first built. READ @0x488d87..0x488dba
(only offsets 5 and 7 written). Harmless: old acks are still true acks.

Sequence/serial arithmetic is u16 with wrap; all comparisons are signed 16-bit differences. READ (the `movzx` +
`test ax, ax` / `cmp ax, 0x20` pairs at 0x486c29, 0x486dfe, 0x486e7b, 0x488737).

---------------------------------------------------------------------------------------------------------------

## 2. Packet types and payloads

| Type | Name | Size | Built by | Sent when |
|---|---|---|---|---|
| 0 | CONNECT | 15 + 45 = 60 | NewConnectPacket 0x488810 | Connecting state, every 1 s (section 3) |
| 1 | ANSWER | 15 + 77 = 92 | NewAnswerPacket 0x4888c0 | Answering state, at once after a CONNECT, then every 1 s |
| 2, 3 | (none) | - | never built | receiver logs "ignoring unimplemented packet of type %d" READ jump table 0x48aa18 -> 0x48a88d |
| 4 | DATA | 15 + 0..497 | ReadPacket 0x488980 | stream data or end-of-stream (payload 0) |
| 5 | ACK | 15 | NewPacket(5) 0x488b20 | delayed ack, when nothing else carried it |
| 6 | KEEPALIVE | 15 | NewPacket(6) | nothing but ACKs sent for 2 s |
| 7 | GOODBYE | 15 | NewGoodbyePacket 0x488aa0 | once, after our end-of-stream is acked and the connection is closed |
| 8 | NAT traversal | any, <= 512 | provider (PrepareTraversalMessage 0x48ba80 puts the 8) | by the NAT traversal provider |

Receiver dispatch: READ @0x48a56b.. (ReceiveData). Order of checks per datagram: size > 0 and byte 0 == 8 -> NAT
queue (before the length checks, so a NAT datagram may be shorter than 15); else size < 15 -> "short packet";
type >= 9 -> unknown; length mismatch; type 0 -> connector ProcessConnect; types 1..7 -> the connection with the same
IP and port whose state < 5 (Errored excluded), none -> "ignoring packet of type %d from unknown host".

### CONNECT payload (45 bytes) READ @0x488864..0x4888b1, checked @0x48aa40

| Payload off | Datagram off | Size | Field |
|---|---|---|---|
| 0 | 15 | u32 | protocol version = **2** (connector rejects others: "connect with wrong protocol (got %d, required %d)") |
| 4 | 19 | u64 | sender's GetTime at build time (microseconds, section 8); used only for the "obsolete" check |
| 12 | 27 | u8 | sender's compression method for ITS sends (conn+0x424): 0 none, 1 deflate; >= 2 is ignored by the receiver |
| 13 | 28 | 32 bytes | sender's nonce (conn+0x454) |

Receiver: total size must be 60 ("wrong length CONNECT") READ @0x48aa60.

### ANSWER payload (77 bytes) READ @0x488913..0x488971, checked @0x4865e0

| Payload off | Datagram off | Size | Field |
|---|---|---|---|
| 0 | 15 | u32 | 2 (not checked by the ANSWER receiver; READ, no compare in 0x4865e0) |
| 4 | 19 | u64 | sender's GetTime |
| 12 | 27 | u8 | sender's compression method for its sends |
| 13 | 28 | 32 bytes | sender's own nonce (conn+0x454) |
| 45 | 60 | 32 bytes | the peer's nonce as received in its CONNECT (conn+0x474) = "receiver nonce" |

Receiver: total size must be 92; "receiver nonce" (datagram 60..91) must equal our own nonce (memcmp of 32 bytes,
ReceiverNonceDiffers32 0x485aa0) else "ANSWER with wrong receiver nonce". READ @0x486716.

**Nonce values.** Our own nonce conn+0x454 is zeroed in the ctor (READ @0x485df0..0x485e1a) and no instruction in
.text or .exxt writes conn+0x454 afterwards (whole-section scan for `+0x454]`, only the ctor and the two readers
0x4888a3 / 0x488953 / 0x486716 hit). **INFERRED: the original always sends 32 zero bytes as its nonce**, so both
nonces on the wire are zero and the check always passes against another original. An implementation must send
zeros to stay identical, and must accept an ANSWER whose receiver nonce equals what it sent.

### DATA payload READ @0x488980

The next `min(pending, 497)` bytes (0x1f1 = 512 - 15) of the connection's outgoing byte stream (section 4). A payload
of length 0 is the **end-of-stream** marker: sent once after the outgoing stream is closed and drained; it consumes a
sequence number and is reliable like any DATA (sets conn+0x50d "Sent Shutdown"). READ @0x488a87.

### ACK, KEEPALIVE, GOODBYE: header only (payload length 0). NAT (8): opaque bytes for the provider.

---------------------------------------------------------------------------------------------------------------

## 3. Connection state machine

States (conn+0x42c; names from the Debug table at 0xdffb8c, READ): **0 Pending, 1 Connecting, 2 Answering,
3 Established, 4 TimedOut, 5 Errored.**

Created by:
- `Connect(ip, port)` 0x48b2b0 (connector vtable +0x10). For each existing connection to that ip:port: Pending ->
  becomes **Answering** and is returned (accepting an incoming one); Connecting / Answering / Established -> set to
  Errored (replaced); Errored -> skipped; TimedOut -> hits the "Reached the supposably unreachable" assert
  (line 0x95b). If none was returned, a new connection in **Connecting** is made. READ @0x48b32c, table 0x48b3f8.
- an incoming CONNECT from an ip:port with no live (state < 5) connection: new connection in **Pending**, then its
  ProcessConnect runs on that packet. Skipped if ctr+0x50 (connector shutting down) is set. READ @0x48abc9..0x48ac19.
- `Accept(ip, port)` 0x48b4f0 is literally `Connect` (tail-jump to vtable +0x10). READ.
- `Reject(ip, port)` 0x48b500: every Pending, not-closed connection to that address gets ScheduleDestroy; **no
  packet is sent** to the rejected peer. READ.
- `FindNextAddress(&ip, &port)` 0x48b410: returns the address of the first connection that is Pending and not
  closed (conn+0xe40 == 0); false if none. This is how the lobby learns of incoming peers. READ.

### Handshake (A connects to B)

1. A: Connecting. SendData sends a fresh CONNECT when `inResponseTo != 0` or `now - lastSend > 1 000 000 us`
   (first one at once, lastSend starts 0); returns "call me again in TimeSince(lastSend + 1 s)" ms.
   READ @0x488367..0x4883c7 (0xf4240 = 1 000 000).
2. B: CONNECT from unknown -> Pending; ProcessConnect stores A's nonce (-> conn+0x474), A's time (conn+0x498),
   A's compression (conn+0x428), and adopts the packet (inResponseTo = A's serial; ack deadline set). B sends
   nothing while Pending. The lobby sees it via FindNextAddress, calls Accept (or Connect) -> Answering.
3. B: Answering. Same rule as step 1 with ANSWER: at once (inResponseTo != 0 after the CONNECT), then every 1 s.
   READ @0x4883e1..0x488443.
4. A: ANSWER in Connecting or Answering, size 92, newer time, right receiver nonce, compression < 2 -> ping sample,
   lastRecv, peer nonce/time/compression stored, **CreateFilterStream: state = Established**, adopt. READ
   @0x48688e..0x4868f2. ANSWER in Pending ("on pending connection") or Established/TimedOut/Errored ("on established
   connection") is ignored. READ @0x4867d5.., @0x48682b..
5. B: the first DATA / ACK / KEEPALIVE / GOODBYE from A while Answering -> CreateFilterStream -> **Established**,
   then the packet is processed normally. READ ProcessAckInternal jump table 0x486d98 (state 2 -> 0x486c0d).

Both sides connecting at once (full mesh, both call Connect): each receives a CONNECT while Connecting -> goes to
Answering (READ jump table 0x4865c0: state 1 -> 0x4864d2 sets state 2); each then receives the other's ANSWER while
Answering -> Established.

CreateFilterStream 0x486910 sets state 3 and first writes a local CMessage of type **201 (0xc9, "connection
made")** into the connection's input stream, so the lobby receives it in order before any peer data. READ @0x486935.

### CONNECT received on an existing connection (CNetUDPConnection::ProcessConnect 0x486380)

- `hdr time <= conn+0x498` (last time seen from the peer; strictly newer required) -> "ignoring obsolete CONNECT",
  returns true (absorbed). READ @0x486395..0x4863b8. Same rule for ANSWER ("obsolete ANSWER") @0x486692.
- compression >= 2 -> ignored. READ @0x486445.
- Pending / Answering: store nonce, time, compression; adopt. Connecting: same, then -> Answering. READ table 0x4865c0.
- **Established / TimedOut: -> Errored**, input marked ended (conn+0xde4), input stream closed, connector event set,
  returns false (the peer restarted). The connector then keeps looking and, finding no live connection, makes a new
  Pending one for the same address. READ @0x48655e, @0x48abb5..0x48abcf.

### Timeouts and keep-alive (all READ in SendData 0x488300, table 0x488718)

- **Connecting / Answering: no timeout at this layer**; CONNECT / ANSWER repeat every 1 s forever. READ (no time
  check other than the 1 s one in those branches). Who gives up is the lobby (open point 1).
- **Established -> TimedOut** when `now - lastRecv > 10 000 000 us` (10 s, 0x989680), log "connection timed out."
  READ @0x488445..0x4884d9. lastRecv = conn+0x440, set on every accepted CONNECT / ANSWER / ACK / KEEPALIVE /
  GOODBYE, and on DATA that is new (put in the early list). READ @0x486433, 0x486897, 0x486f50, 0x48731e, 0x487352,
  0x4873ac.
- TimedOut: only KEEPALIVEs (every keep-alive interval), no DATA or resends. **Any DATA/ACK/KEEPALIVE/GOODBYE that
  passes the header check turns it back into Established** (READ table 0x486d98: state 4 -> 0x486c15 sets 3). No
  local message is emitted for TimedOut (READ: no CMessage built in these paths); the lobby sees it as "quiet".
- **Keep-alive interval** conn+0x450 = 2 000 000 us (2 s, 0x1e8480). Only ever set to that value (ctor 0x485de6,
  SendPacket of a KEEPALIVE 0x489101, UpdatePingInfoFromPacket 0x487573), so it is constant. KEEPALIVE is sent when
  `now >= lastKeepAliveStamp + 2 s`, where the stamp (conn+0x448) is the time of the last sent packet of any type
  **except ACK** (READ @0x4890eb). So a connection that sends data never sends KEEPALIVEs.

### Close and goodbye

- `Close()` (vtable +0x14, 0x4893f0): if not already: finish the deflate stream (Z_FINISH, section 6) and close the
  outgoing stream; conn+0x50c "Output Shutdown" = 1. READ.
- `ScheduleDestroy()` (vtable +0x1c, 0x489660): Close(), then conn+0xe40 "Closed" = 1. READ.
- Established + Closed: remaining data goes out, then the 0-length end-of-stream DATA (HasPacketWaiting: output
  empty and its stream closed and not yet sent, READ @0x488749..0x488774). When `Sent Shutdown && unacked list
  empty`, one GOODBYE is sent and conn+0xe41 set. READ @0x488591..0x4885ad. GOODBYE is never resent.
- Any other state + Closed -> conn+0xe41 at once, nothing sent (READ 0x48833f, 0x488367, 0x4883e1, 0x4884e3).
- conn+0xe41 set -> SendData returns "never" and `Pull` (main thread) deletes the connection. READ @0x48b764.
- GOODBYE received (0x487370): header processed; if the peer's end-of-stream was not received yet -> **Errored**,
  input ended, inflater dropped, event set. If it was, nothing more. READ.
- End of input (DispatchFromInput 0x4876a0): once the input stream is empty and ended, one local message: **202
  (0xca) if state is Errored**, else **203 (0xcb)** (clean end-of-stream). READ @0x48794e..0x4879ad.

---------------------------------------------------------------------------------------------------------------

## 4. Reliability

**Sending side.**
- Outgoing byte stream: `Write(CMessage)` 0x489130 appends the message's bytes (`[u8 type][u16 size LE, header
  included][payload]`) to the deflater (if on) or straight to the output pipe (conn+0x4b8). If the send deadline
  conn+0x4a8 is 0 it becomes `now + net_SendDelay` (25 ms). Sets conn+0x504 (needs flush). Wakes the connector
  thread. READ.
- `Push()` 0x48b7f0 (connector, main thread): for each connection not shut down, flush the deflater (sync flush)
  and the pipe, conn+0x508 "Flushed Output Data" = pipe length; wake the thread. READ. Flushed data is sent at once
  (below); unflushed data waits for the 25 ms deadline.
- DATA packets are cut from the pipe in order, 497 bytes max, no relation to CMessage boundaries; the receiver
  re-frames the byte stream (section 4, receiving). READ ReadPacket.
- **HasPacketWaiting(now)** 0x488730, in order: (a) `(i16)(nextSeq - remoteExpected) > 32` -> no (window full);
  (b) pipe empty, pipe closed, end-of-stream not sent -> yes (send the 0-length DATA); (c) if `now > sendBy` or
  flushed > 0 or `now >= keepAliveStamp + 2 s`: sync-flush deflater and pipe if conn+0x504, clear it, -> yes if
  pipe has >= 1 byte; (d) otherwise -> yes only if >= 497 bytes are waiting (a full packet). READ.
  Note `sendBy == 0` makes `now > sendBy` true, so with no deadline any pending byte is sent. READ (signed compare
  0x48877e).
- **Window**: the sender may have seqs `remoteExpected .. remoteExpected+32` outstanding (33 packets); matches the
  receiver's limit. READ @0x488737, 0x486e7b.
- After every send (SendPacket): serial += 1, inResponseTo = 0, `sendBy = pipe non-empty ? now + 25 ms : 0`,
  history slot `[serial % 128]` = {serial, timer reset} (16-byte slots at conn+0x518, timer at slot+8), backlog +=
  datagram size, lastSend = now, keep-alive stamp = now unless ACK. READ @0x48901b..0x4890fc.
- **Unacked list** conn+0x510: every sent DATA (incl. end-of-stream) goes in, sorted by resend time ascending
  (stable: equal times go after). Resend time = `now + CalcResendDelay`, then the packet's send count += 1. Other
  types are freed after sending. READ @0x488e59..0x488ebf.
- **CalcResendDelay** 0x488170: `ms = (int)round((ping_ms * net_ResendPingMultiplier + net_ResendDelayBias)
  * 2^min(sendCount, 3))`, then `ms = min(ms, net_MaxResendDelay)`, then `ms = max(ms, net_MinResendDelay)`,
  result in microseconds (`* 1000`). sendCount is 0 at the first send, so the factors are 1, 2, 4, 8, 8, ...
  READ. With the defaults: `clamp((ping + 25) * f, 100, 1000)` ms. The rounding is `fistp` with the FPU's current
  mode, round-to-nearest by default (INFERRED: the game does not change the FPU rounding mode; not checked).
- **Ack**: an ACK goes out only when `inResponseTo != 0 && now >= sendBy`, where AdoptPacket (on CONNECT, ANSWER,
  DATA, KEEPALIVE; not ACK, not GOODBYE) set `sendBy = packetRecvTime + net_AckDelay (25 ms)` if it was 0.
  READ 0x488220, 0x4885d9. Any other packet sent before then carries the same ack fields and resets
  inResponseTo/sendBy, so the ACK is skipped (piggybacking). READ SendPacket 0x48901b..0x48907d. Repeated old
  DATA is still adopted (AdoptPacket runs before the seq check, READ 0x486de8), so a lost ack is repaired.
- **SendData priority per call** (one packet per call, Established): due resend (front of the unacked list with
  resend time <= now) > new DATA (HasPacketWaiting) > GOODBYE > KEEPALIVE > ACK. Return value: 0 = call again now
  if flushed > 0 or > 497 bytes pending; else the ms until the earliest of keep-alive due, front resend due, and the
  send deadline (the last only if it is set and inResponseTo != 0 or data is pending or a flush is needed); -1 =
  never (destroyed). READ @0x48854a..0x4886d1.
- **Rate limit** (connector send pass 0x48ac40): per connection a leaky-bucket "backlog" (conn+0x438, bytes) that
  drains at `net_MaxSendRate` bytes per second (`backlog - (now - lastSend) * rate * 1e-6`, truncated, floored at
  0; GetSentTime 0x488260). SendData is called only while `backlog <= net_MaxBacklog`; otherwise the thread waits
  `(backlog - maxBacklog) * 1000 / rate` ms. READ @0x48af60..0x48afe2. With the defaults (2048 / 2048) that is a
  2 KB burst and 2 KB/s per peer.

**Receiving side** (ProcessData 0x486db0, after the header check of section 1):
- `delta = (i16)(hdr.seq - expected)`: < 0 -> "repeat of old DATA" dropped; > 32 -> "DATA from too far in the
  future" dropped; 0..32 -> inserted into the **early-packet list** (conn+0xd8c) sorted by seq; an equal seq already
  there -> "repeat of future DATA" dropped. READ @0x486dfe..0x486f2c.
- Then, while the list's front has `seq == expected`: payload > 0 -> written to the inflater (if on) else straight
  to the input pipe (conn+0xd98); payload 0 -> end of stream (close the inflater, close the input pipe, conn+0xde4 =
  1); expected += 1. Packets after end-of-stream are dropped ("DATA after end-of-stream"). READ @0x486f70..0x487298.
- Then the ack mask is rebuilt from the list: `mask = OR of 1 << (seq - expected - 1)`. READ @0x48729d..0x4872db.
- If anything was delivered, the connector event is set (the main thread then runs Pull). READ @0x4872dd.
- `Pull()` 0x48b5c0 (main thread): hands queued NAT packets to the provider, deletes finished connections, runs
  FlushInput (sync-flush the inflater so its output reaches the pipe, 0x487590) and DispatchFromInput for each
  connection. READ (call order 0x48b764..0x48b7af).
- **DispatchFromInput** 0x4876a0: repeatedly `CMessage::Read` from the input pipe: reads the 3-byte header, size
  = bytes 1..2 LE including the header, size < 3 -> not a message (Read returns false and nothing more is read),
  else waits for `size` bytes; each whole message goes to the receiver registered for its type (conn+0xc +
  type*4), "No receiver for message type %d received from %s:%d" if none. READ @0x47bee0, @0x48786f. Then end of
  input -> 202/203 (section 3).

---------------------------------------------------------------------------------------------------------------

## 5. Ping and "quiet"

- **Ping sample**: on every packet that passes the header check (DATA, ACK, KEEPALIVE, GOODBYE via
  ProcessAckInternal) and on an accepted ANSWER: if `hdr.inResponseTo != 0` and history slot `[irt % 128]` holds
  that serial, sample = milliseconds (float) since that serial was sent (gpg Timer, QPC). READ 0x4874c0. The sample
  includes the peer's ack delay (up to 25 ms) because the peer answers whenever it next sends (INFERRED from the
  AdoptPacket / SendPacket logic above).
- Samples go into a ring of 25 slots holding the last **24** samples (0x48bec0: full when next == head, then the
  oldest is dropped). **Ping = median**: the samples sorted, element `[n/2]` (upper median). Jitter = the same
  median of `|sample - ping|` (logged only). READ 0x48bf00, 0x48bf60; sort routine 0x48cff0 not read (INFERRED
  ascending sort from the use). Stored as float at conn+0xd88; `GetPing()` 0x489550 returns it (0 before the first
  sample, ctor 0x485eed). Each sample also resets the keep-alive interval to 2 s (no effect, it is always 2 s).
- The u64 times in CONNECT/ANSWER are **not** used for ping; only for the strictly-newer ("obsolete") check. READ.
- **Quiet** = `CNetUDPConnection` vtable slot 3 (0x489590, named GetTime): `(now - lastRecv) * 0.001` ms as float,
  or **-1.0** if nothing was ever received. READ. (This is the lobby's `quiet` value, survey line 130.)

---------------------------------------------------------------------------------------------------------------

## 6. Compression

- Per direction, chosen by the **sender**: at connection creation conn+0x424 = `net_CompressionMethod` (default
  **1 = deflate**, 0xf58df8 = 1); any other value -> warning "Unknown compression method %d, assuming none." -> 0.
  READ ctor 0x485da4..0x486004. The value is announced in CONNECT/ANSWER byte 27; the receiver stores it at
  conn+0x428 and, in CreateFilterStream, makes an inflater over its input pipe if it is 1 ("NET: using deflate
  compression for receives from %s"). READ 0x4869b1.
- It is **one continuous stream per direction for the whole connection**, not per packet: the sender's deflater
  (ZLibOutputFilterStream, mode 1) sits in front of the output pipe for the connection's life; DATA payloads are
  slices of its output. READ ctor 0x486052..0x48609d, Write 0x48917b.
- Parameters: zlib "1.2.3", `deflateInit2(level -1 (default), Z_DEFLATED, windowBits -14, memLevel 8, strategy 0)`
  = **raw deflate, 16 KB window, no zlib header or checksum**; receive side `inflateInit2(windowBits -14)`.
  READ @0x957432..0x957443, 0x95748c..0x957496.
- Flush points: stream Flush = DoWrite with flush code 2 (**Z_SYNC_FLUSH**) READ 0x957884; Close = code 4
  (**Z_FINISH**) READ 0x9578e1. The sender sync-flushes on `Push()` and when the 25 ms send deadline (or a
  keep-alive) is due with unflushed data (HasPacketWaiting step c); Z_FINISH on Close, before the end-of-stream
  DATA. INFERRED that DoWrite passes the code to deflate()/inflate() as zlib's flush argument (DoWrite 0x957500 not
  read).
- Errors: an inflate failure ("Error decompressing data from %s: %s") or an end-of-stream before the deflate
  stream's end ("ZLibOutputFilterStream: stream closed before end.") -> Errored, input ended. READ 0x48710b..,
  0x4871e9.., 0x4878fd.

---------------------------------------------------------------------------------------------------------------

## 7. Connector: socket, demultiplexing, NAT

- Socket: `socket(AF_INET, ?, IPPROTO_UDP 0x11)`, non-blocking (`ioctlsocket FIONBIO`), bound to the local port
  (0 = any), READ NET_MakeUDPConnector 0x48bbe0. One WSA event on the socket for FD_READ|FD_WRITE (3) READ
  0x489802. A connector thread "CNetUDPConnector for port %d", priority 2 (THREAD_PRIORITY_HIGHEST) READ 0x489ffd,
  loops: ReceiveData (all waiting datagrams, until WSAEWOULDBLOCK 0x2733), send pass, then
  `WSAWaitForMultipleEvents(1, ev, FALSE, timeout, TRUE)` with the smallest wait any connection asked for (-1 =
  infinite). READ 0x48a048..0x48a1ca. All connection state is under the connector's lock (ctr+8). READ.
- recvfrom buffer 512 (0x200) bytes; larger datagrams are truncated by Winsock (INFERRED, recvfrom then fails with
  WSAEMSGSIZE and the loop logs and stops; not traced). Each received packet is stamped with GetTime (pkt+8) at
  receipt; that stamp is "lastRecv" and the ack-deadline base. READ 0x48a374..0x48a3d3.
- **Demultiplexing is by source IP and port only** (connection vtable 0 / 1 = conn+0x41c / conn+0x420), skipping
  Errored connections. Nonces are not used to find a connection. READ 0x48a79d..0x48a7ea, 0x48ab70..0x48abb3.
- Unknown address: CONNECT -> new Pending connection (section 3); anything else is dropped. READ.
- **NAT traversal (type 8)**: incoming datagrams with first byte 8 are queued (ctr+0x68, with source ip/port) and
  the event set; Pull hands them to the NAT traversal provider (ctr+0x1c). READ 0x48a573..0x48a5da; Pull's provider
  call 0x48b613 (INFERRED that this is the hand-over; the provider call itself not traced). Outgoing: the provider
  builds a message whose first byte PrepareTraversalMessage sets to 8, and gives it to the connector (secondary
  vtable, ReceivePacket 0x48bae0: "Truncating NAT traversal packet from %d to %d bytes" over 512); it is queued and
  sent raw, before the connections, in the next send pass. READ 0x48ba80, 0x48bae0, 0x48ac74..0x48acd6. At
  creation the connector tells the provider its local port. READ 0x4898dd..0x489939.
- Packet buffers: 0x218-byte objects, a pool of up to 20 (0x14) free ones. READ 0x48a990. Not protocol.
- SnapshotSendStamps 0x48b9e0: bandwidth statistics over the last N ms (stats ring at ctr+0x80). Not protocol.

---------------------------------------------------------------------------------------------------------------

## 8. Time

- `CNetUDPConnector::GetTime` 0x489f30 = `base + elapsed`, in **microseconds**: base = `GetSystemTimeAsFileTime /
  10` taken at connector creation (microseconds since 1601-01-01 UTC, READ 0x4897dc..0x4897fc), elapsed = a
  gpg::time::Timer (QueryPerformanceCounter / QueryPerformanceFrequency, READ 0x955400, 0x95553b) converted with
  CyclesToMicroseconds. Forced **strictly increasing**: if not greater than the last value returned, last + 1.
  READ 0x489f59..0x489f7c. This is the clock of every conn timestamp and of the u64 in CONNECT/ANSWER.
- Ping samples use the gpg Timer in milliseconds (float). READ 0x4874fd.
- Wait timeouts handed to WSAWaitForMultipleEvents are milliseconds; TimeSince 0x4881f0 actually returns
  `max(0, (t - now) / 1000)` ms, i.e. time UNTIL t. READ.

### Constants (console variables, defaults in .data; registered at 0xbc4ec0..0xbc5150, names READ from 0xf5928c..)

| Var | Address | Default | Used |
|---|---|---|---|
| net_DebugLevel | 0x10a6384 | 0 | log spew |
| net_AckDelay | 0xf58de0 | 25 ms | AdoptPacket |
| net_SendDelay | 0xf58de4 | 25 ms | Write, SendPacket |
| net_LogPackets | 0x10a6381 | 0 | packet log file |
| net_MinResendDelay | 0xf58de8 | 100 ms | CalcResendDelay |
| net_MaxResendDelay | 0xf58dec | 1000 ms | CalcResendDelay |
| net_MaxSendRate | 0xf58df0 | 2048 bytes/s | backlog drain |
| net_MaxBacklog | 0xf58df4 | 2048 bytes | send gate |
| net_CompressionMethod | 0xf58df8 | 1 (deflate) | ctor |
| net_ResendPingMultiplier | 0xf58dfc | 1.0f | CalcResendDelay |
| net_ResendDelayBias | 0xf58e00 | 25 ms | CalcResendDelay |

Fixed in code: CONNECT/ANSWER repeat 1 s (0xf4240 @0x488399), timeout 10 s (0x989680 @0x488470), keep-alive 2 s
(0x1e8480), window 32, max payload 497, datagram 512, protocol 2, send history 128 slots, ping ring 24 samples.

---------------------------------------------------------------------------------------------------------------

## 9. FAF's patch (dormant by default)

The `.exxt` copies of SendPacket (0x128ecf0) and ReceiveData (0x128eb30) add an in-band marker in **header bytes 2
and 3** (bits 8..23 of the ack mask), gated by FAF globals 0x11fd243 / 0x11fd23f (both 0 at start, set from FAF
Lua code around 0x128fa97 / 0x128fb12). READ:
- sender with 0x11fd243 == 1 and those bytes 0: writes byte2 = 0x4e, byte3 = 0xa4.
- receiver with 0x11fd243 != 1 seeing 0x4e/0xa4: zeroes them, sets 0x11fd23f = 1, remembers the sender
  (0x1292b60); it then marks its packets to that peer with byte2 = 0x18, byte3 = 0xfe (0x128f0eb).
- receiver with 0x11fd243 == 1 seeing 0x18/0xfe: zeroes them and records the sender in a FAF table.
A receiver without this patch would read those bytes as selective acks and could drop unacked DATA early, so the
feature only works between FAF exes. Not needed for original compatibility; purpose not read (open point 5).

---------------------------------------------------------------------------------------------------------------

## Open points

1. **Who gives up on a connection that never answers.** The UDP layer retries CONNECT every 1 s with no limit and
   builds no 200 (0xc8) message anywhere (whole-.text scan: CMessage ctors with 0xc8 are absent; 0xc9..0xcb only at
   0x483b42, 0x4842fd, 0x48442b (TCP), 0x4854e0, 0x48694b, 0x487971, 0x48799e). The "connection failed" / give-up
   timing is in the lobby (CLobby), not read here.
2. TimedOut emits no message; how the lobby reacts (FAF lobby.lua's 30 s quiet timeout is script side) is not read.
3. CMessage receivers for 201..203 are dispatched through the connection's receiver table; the lobby's handling
   is in the survey, not re-read.
4. zlib DoWrite (0x957500) and the PipeStream flush/close semantics (vtable +0x18 "closed?", +0x20 Flush, +0x24
   Close(2)) are INFERRED from their use and the flush codes, not read in full.
5. FAF's marker feature (section 9): what it is for and which Lua function sets 0x11fd243 / 0x11fd23f.
6. Rounding mode of `fistp` in CalcResendDelay (assumed round-to-nearest), and the sort at 0x48cff0 (assumed
   ascending) for the median.
7. Whether anything writes the local nonce through a pointer that the `+0x454]` scan cannot see (e.g. a memcpy
   into the object from outside). None found; zeros are the working assumption. A capture of two original exes
   would settle it.
8. NAT packet hand-over to the provider in Pull (0x48b5fd..0x48b74a) and the provider interface were not traced
   past the call.
