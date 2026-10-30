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
