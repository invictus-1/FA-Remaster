# A network game's control messages: what the original does (network step 5, 2026-10-30)

Read in the lab's pinned exe (FAF's patched ForgedAlliance.exe, build 3839) for `ui/net/beat_exchange_net.*` and
the network parts of `ui/app/game_session.cpp` and `ui/user/session_bindings.cpp`. READ means seen in the code at the
address named; INFERRED is marked. The stream itself (types 0..49) is the engine's (`src/sim/lockstep.h`,
engine-ref `command_stream.md` 1.1).

## The client manager's shape

- `CClientManagerImpl` (vtable 0xe16b64) holds one `CClientBase` per client (+0x420 vector): `CLocalClient` (this
  program), `CNetClient` (a peer over its connection, +0xe4), `CNullClient`, `CReplayClient`. A client object's
  `Process(msg)` (slot 7) means "give this client the message": a `CNetClient` writes it to its connection
  (0x53dd60), the `CLocalClient` handles it at once as if it came from itself (0x53d190 -> `CClientBase::Process`
  0x53bf30). The manager's broadcast (slot 17, 0x53e990) calls every client's `Process`, so every broadcast is also
  handled here. Messages arriving from a peer go to `CClientBase::Process` with that peer's object (0x53de70).
- Per client (READ, ctor 0x53bd40): +0x20 index, +0x50 command source now, +0x54 ready, +0xa0 its beats in (Advances),
  +0xa4 its beats handed to the sim, +0xa8 its AVAILABLE, +0xb0 its ACKs per client, +0xbc its DISPATCHED, +0xc0
  ejected (by this program), +0xc1 done (its sources terminated), +0xc4 eject requests against it ({requester, beat}),
  +0xd4 its message 57 value (50 at the start; FAF shows it as `maxSP`).
- Per manager (READ, ctor 0x53df20): +0x418 the interface (`CWldUiInterface`, vtable 0xe4a938), +0x430 the local
  client, +0x434 READY sent, +0x435 everyone ready, +0x438 dispatched, +0x43c available beat, +0x440 / +0x444 fully /
  partially queued, +0x448 speed request serial, +0x44c its client, +0x450 the speed asked for, +0x454 speed
  adjustable (both from the game's options).

## The control messages (`CClientBase::Process` 0x53bf30, jump table 0x53c31c, index table 0x53c344)

| Type | Payload | On receipt | Sent by |
|---|---|---|---|
| 0 Advance | i32 n | +0xa0 += n, then ACK broadcast {its index, +0xa0} (0x53bfda) | each client's stream |
| 50 ACK | u8 client, i32 n | sender's acks[client] = n if larger ("ignoring out of sequence ACK ...", "ignoring ACK ... for invalid client") | on every Advance taken in |
| 51 DISPATCHED | i32 beat | sender's +0xbc (not smaller) | UpdateStates (FAF's 0x12900c0) per beat |
| 52 AVAILABLE | i32 beat | sender's +0xa8 (not smaller) | DoBeat 0x53eb91 when the available beat moved |
| 53 READY | none | sender's +0x54 = 1 | slot 11, 0x53e4b0, called at the end of DoInitializing (0x88c66d) |
| 54 EJECT | u8 client, i32 n | HandleIncomingEjectRequest 0x53f440: that client's AddOrUpdateEjectRequest(sender, n) (one per requester, the smaller n) | ProcessEject 0x53f2c0 |
| 55 CHAT | the message's bytes | the interface's ReceiveChat (CWldUiInterface 0x88b880 -> 0x83ebd0: gamemain.ReceiveChat(sender's name, message)) | slot 8, 0x53c9a0 |
| 56 SPEED | i32 serial, i32 rate | where +0x454: ApplyIncomingGameSpeedRequest 0x53e810 | SetSimRate 0x53e590 |
| 57 | i32 | sender's +0xd4 | BroadcastIntParam 0x53e850; FAF's sim driver sends its measured rate (0x129004e) |
| 202 / 203 | (connection error / end) | the client's Eject (slot 10) at once, then the interface's NoteDisconnect ("<LOC Engine0002>%s disconnected.", 0x88b9d0) (0x53c2c3) | the connection |

## When a beat runs (DoBeat 0x53ea30)

The available beat (+0x43c) moves on while every client is `IsReadyForBeat` (0x53c3e0) for the next one
(EveryoneResponsiveSince 0x53ef30): a client that is done or ejected always is; otherwise it must have acknowledged
every client's part of that beat, a client done being skipped and an ejected one from its last beat on
(GetMostExpiredEjectRequest 0x53cc20: the smaller of its beats in here and every eject request's beat). The sim runs
beats up to it (GetAvailableBeat slot 21). So a beat runs only when everyone still in has everyone's part of it.

## Eject (EjectSessionClient 0x88d970, CClientBase::Eject 0x53cb10)

- EjectSessionClient(i): one integer 1 .. clients, not the local one ("Can't eject ourselves!"), then client i's
  Eject: ejected = ready = 1, its connection dropped (CNetClient slot 14, 0x53de20), ProcessEject: EJECT {i, its beats
  in here} broadcast (so this program's own request is recorded too), and the requests the ejected client made are
  removed everywhere (0x53cd50).
- Each program terminates the client only once it has ejected it itself: in UpdateState (0x53c5f6) once its beats
  handed to the sim reach its last beat, a SetCommandSource + CommandSourceTerminated per source (0x53c830), done = 1.
- So every client still in must eject it (the "vote"): one that has not still waits for its beats, and the others
  wait for that one's ACKs. GetSessionClients' `ejectedBy` lists who asked; `connected` is "not ejected here".
- The ejected program hears nothing (its link is dropped before the broadcast); its peers' connections end, and the
  original then ejects them all at once (202/203), going on alone.
- Ours does the same (Chris, 2026-10-30: "Just keep faf behaviour"): a connection that ends ejects its client at once
  and prints "<LOC Engine0002>%s disconnected."; a client gone quiet with its link up waits for the dialog's vote.
- Ours differs in one thing: an ejected client's last beat itself waits for everyone's ACK (the original skips it:
  INFERRED to be a one-beat window for a desync with three or more players).

## Readiness and the waiting dialog (WLD_Frame 0x88cae0)

DoInitializing (0x88c3f0) ends with READY (0x88c66d) and state 4. DoPostInitializing (0x88c6d0): if
IsEveryoneReady (0x53e560: +0x435, set by DoBeat once +0x434 and every client's +0x54) the game plays (state 6),
else the provider's StartWaitingDialog (0x88c72d) and state 5. DoWaiting (0x88c750): once everyone is ready,
StopWaitingDialog and play; until then uimain.UpdateDisconnectDialog each frame (0x88c7b8). A player lost while the
others load never sends READY: the others wait, the disconnect dialog shows it once quiet, and its eject (ready = 1)
lets the game start.

## Game speed

SetGameSpeed (0x88e360) -> SetSimRate (slot 13): where +0x454, SPEED {serial + 1, rate} broadcast. Every client takes
a request whose serial is newer, or equal from a lower client (0x53e810), and tells the interface
(uimain.NoteGameSpeedChanged(client + 1, rate), 0x83d670). GetGameSpeed = the rate asked for (slot 15, +0x450); the
rate run (slot 14, 0x53e720) is the smaller of it and every client's 57 value.

## Chat

SessionSendChatMessage (0x88db00): `[clients,] message`; clients an integer or a table of them (1 .. clients), left
out every client including this one; the message serialized (ToByteStream), at most 1024 bytes; then each named
client's slot 8 (CHAT 55; this program's own back to itself).

## The desync dialog

CWldSession::DoBeat 0x894530: for a sync with desyncs, one string per desync record (the client's name, 0x895364),
GPGNET_ReportDesync, then UI_ShowDesyncDialog (0x895434) -> uimain.ShowDesyncDialog(the session's beat, strings).

## FAF's /lua/ui/dialogs/disconnect.lua (build 3839), left as it is

The dialog opens when a client's `quiet` > 5000 ms, or a connected client has a non-empty `ejectedBy`, or a client
not connected has an `ejectedBy` that differs from the connected clients. Its timer starts when it opens: with D =
DisconnectionDelay02 (default 90; "10", "30", "90"), the Eject buttons of quiet clients are enabled after D - 5 s;
after max(2 (D - 5), 85) s the dialog calls EjectSessionClient for every client still quiet (> 5000 ms) by itself; and
an observer (or a defeated player: armiesTable[own index].outOfGame) ejects every quiet client by itself as soon as
the buttons would be enabled. Only connected clients with quiet > 5000 are ejected that way; a client shown "Not
Connected" (ejected here) gets no button.

## A link with latency (netlat, 2026-10-30)

Does a game stutter over a link with 100-150 ms of round trip, now that a beat waits for everyone's ACKs? No: up to
300 ms of round trip not one beat waited for the network. Measured with three test-only switches, all off by default:

- `MOHO64_NET_DELAY_MS=<ms>` (`ui/net/test_delay.h`, both connectors): everything this program receives is held that
  long before it is taken in (UDP: each datagram, on the connector's thread, which wakes for the held ones; TCP: each
  read, at the next Poll), so with both programs set the round trip grows by twice that. `MOHO64_NET_JITTER_MS=<ms>`:
  0..that much more each, from a fixed seed, never out of order.
- `MOHO64UI_REAL_TIME=1` (main.cpp): a run without a window steps its frames at the PC's clock, at most 60 a second,
  each the time since the last (at most 0.25 s), exactly as a window's loop hands its frame time to
  `GameSession::Advance` (run_window.cpp); the beats' clock then runs at real time as in a live game. Without it a run
  without a window makes each frame 1/60 s of the game however fast the frames go (the engine's 2.4 ms per beat of
  waiting came from that unthrottled loop).
- `MOHO64UI_NET_BEAT_LOG=1` (game_session.cpp, NetBeatOrders): a line per beat with the time since the last beat
  began, the time from its first try to its start, and how much of that with every client's orders already in (that
  part is the wait for the ACKs).

The runs: `ui/tests/net_game_host.lua` / `net_game_join.lua` with `NET_GAME_RUN_TO = 700` (a plain game to beat 700
after the orders), over UDP, both programs on CPUs 16-31, beats 20..700 (68 s) of each program. Ping is what
GetSessionClients shows (the connector's own, with its 25 ms ack delay in it). Gaps over 100 / 120 ms come from the 60
frames a second (a beat lands on the 5th, 6th or 7th frame), the same at 0 ms; the share swings run to run.

| Delay each way (round trip) | Ping, host / joiner | Gaps > 100 / 120 / 150 ms | Worst gap | Beats that waited > 1 ms | Waited in all (of it for ACKs) |
|---|---|---|---|---|---|
| 0 (0) | 30 / 31 | 42 / 12 / 0 % | 142 ms | 4 / 2 | 0.2 s (0) |
| 50 (100) | 153 / 124 | 45 / 11 / 0 % | 129 ms | 2 / 1 | 0.1 s (0) |
| 50 again | 152 / 138 | 23 / 21 / 0.1 % | 160 ms (a slow frame: 11 ms of it a wait) | 13 / 1 | 0.2 s (0) |
| 75 (150) | 170 / 184 | 23 / 22 / 0 % | 128 ms | 3 / 3 | 0.1 s (0) |
| 75 again | 171 / 171 | 23 / 21 / 0 % | 129 ms | 0 / 0 | 0.1 s (0) |
| 75 + 0..30 jitter | 206 / 210 | 65 / 0.4 / 0 % | 145 ms | 7 / 2 | 0.2 s (0) |
| 150 (300) | 326 / 339 | 25 / 20 / 0 % | 127 ms | 1 / 1 | 0.1 s (0) |
| 250 (500) | 535 / 529 | 50 / 19-26 / 1.6-4.0 % | 190 / 310 ms (the joiner's at beat 25) | 669 / 613 | 31 / 28 s (30 / 27 s) |
| TCP, 75 (150) | 310 / 250 | 54 / 6 / 0 % | 127 ms | 0 / 1 | 0.1 s (0) |

Why: a client's orders for beat b + 5 go out when it first asks for beat b (Lockstep::Ready), 500 ms before they are
due, and the beat needs the others' ACK of them: one round trip, plus up to a frame at each end and the connector's
send and ack delays (the ping above). That fits in 500 ms up to about 470 ms of ping; at 530 ms every beat waits
20-35 ms for its ACKs (its orders all in), the game's clock runs on and the beats keep their pace a little behind it.
The fingerprints of host and joiner were equal at every beat of every run.

What the original does (READ, the pinned exe): the beats are ACK-gated too. IsReadyForBeat (0x53c3e0) compares the
client's ACK of every other client with the beat (0x53c44e..0x53c459); DoBeat moves the available beat (+0x43c) while
EveryoneResponsiveSince (0x53ef30) holds (0x53eb5c..0x53eb8f); the sim driver's thread (CSimDriver::ThreadRun 0x73bdf0)
lets the sim run its next beat (+0x20) only once GetAvailableBeat (slot 21, 0x53efd0) reaches it (0x73c11a..0x73c146:
+0x8c 1 -> 2, the event +0x74; Dispatch 0x73c387 then runs it, 0x73d8c0 -> FAF's 0x128fde0, UpdateStates 0x128ff1f).
Its own beats go out by the clock: the same thread compares now with the time of the last sync the interface took
(GetSyncData 0x73c5cc..0x73c5e3 sets +0x1c its beat, +0x50 the time) plus (beats sent - that beat) x ms per beat - net_Lag
(0x73bfbc..0x73c02a; net_Lag's value at 0xf58134, 500, its console variable made at 0xbdb900), and sends the beats now
due (0x73c03a..0x73c10d, at most min(2000 / ms per beat, 99) beats past the last sync), else sleeps until the next is
due. So the original too sends a beat's orders net_Lag ahead and runs it only when everyone has ACKed it; INFERRED (not
measured): it waits likewise once the round trip nears net_Lag. Nothing is changed in the game's code for this.
