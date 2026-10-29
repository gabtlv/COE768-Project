# COE768 P2P Project

Files: `p2p.h` (PDU format and shared constants), `index_server.c` (UDP index server), `peer.c` (peer: content client and content server), `Makefile`.

## Build

```
make            # or:  gcc -Wall -o index_server index_server.c && gcc -Wall -o peer peer.c
```

## Run

```
./index_server [port]              # default UDP port 3000
./peer <index_host> [port]         # asks for a peer name (max 10 chars)
```

Run each peer from **its own directory**. Content names are file names in that directory (max 10 characters), and a download is saved under the same name, so it would overwrite the original if two peers shared a directory.

Peer commands:

| Cmd | Action | PDUs exchanged |
|---|---|---|
| `R <content>` | Open a TCP socket (OS picks the port) and register the content | R → A or E |
| `D <content>` | Search, download over TCP, then automatically register | S → S/E, then D → C/E, then R → A |
| `O` | List all registered content | O → O… (an empty O marks the end) |
| `T <content>` | De-register one content | T → A/E |
| `L` | Show this peer's own contents and their TCP ports | (local) |
| `Q` | De-register everything, then quit | T → A for each |

## Demo script (matches the demonstration requirements)

```
mkdir p1 p2 p3; cp somefile.txt p1/song.txt
T0$ ./index_server
T1$ cd p1 && ../peer 127.0.0.1      name: Alice   →  R song.txt          (1a)
T2$ cd p2 && ../peer 127.0.0.1      name: Bob     →  D song.txt          (1b, 1c, 1d: auto-registers)
T3$ cd p3 && ../peer 127.0.0.1      name: Carol   →  D song.txt          (1d: served by Bob, the least used)
T3:  O                                                                   (1e)
T1:  Q   → index server log shows T / A for each of Alice's contents     (1f)
```

Name conflict: start another peer called `Alice` and run `R song.txt`. The index server replies E, and the peer asks for a new name.

On the lab machines, run the index server on one router and pass its IP (e.g. `../peer 10.1.1.4`). Each peer works out its own IP automatically.

## Where the required parts are in the code (demo item 2)

| Feature | peer.c | index_server.c |
|---|---|---|
| a. Content registration | `register_content()` ~L146 (TCP socket with port 0 + `getsockname` ~L187; retries with a new name on E) | `do_register()` ~L69 (name-conflict check, stores address) |
| b. Content download | `download_content()` ~L229 (S, TCP connect, D, receive C until the connection closes, auto-register); `serve_download()` ~L334 (sends C in chunks, then closes) | `do_search()` ~L125 (picks the least-used server; ties go to the earliest registration) |
| c. Content de-registration | `deregister_content()` ~L406; `quit()` ~L483 de-registers everything | `do_deregister()` ~L164 |
| select() loop | `main()` ~L570: waits on stdin and every content's TCP listening socket | — |

## Design choices (useful for the report)

- **Fixed-size fields** in the Data field: peer name 10 B, content name 10 B, IP 16 B (dotted ASCII), port 6 B (ASCII). An R PDU is 1 + 42 bytes. ASCII keeps the PDUs readable in Wireshark.
- **Peer IP discovery:** the peer `connect()`s its UDP socket to the index server and calls `getsockname()` on it, which gives the IP of the interface that reaches the server. The spec notes that `getsockname` on the TCP socket only gives 0.0.0.0. As a fallback, the index server uses the datagram's source IP if the field is empty or 0.0.0.0.
- **Least-used selection:** each registration has a use counter. A search returns the matching entry with the lowest count (ties go to the earliest registration) and increments that count.
- **O-list over UDP with a 100 B limit:** each O reply holds up to 5 (peer, content) pairs, and the list ends with an O that has an empty Data field.
- **C-type transfer:** same as Lab 3. The first TCP segment starts with `'C'`, the rest is raw data, and the server closing the connection marks the end. If the content isn't available, the server sends `'E'` + a message instead.
- **Concurrency:** uploads are served in a forked child (as in Lab 3), so the user prompt and other uploads aren't blocked. SIGCHLD is reaped, and `select` is restarted on EINTR.
- UDP replies have a 3 s receive timeout, so a peer doesn't hang if the index server is down.

## Tested

Index server plus 5 peers on one machine: register, download, and auto-register chain Alice → Bob → Carol → Dave (the least-used server was chosen each time), name conflict (Alice → Alice2), content not found (E), O listing, T, and Q de-registering everything. Downloaded copies of a 1 MB binary file matched the original byte for byte (`cmp`).
