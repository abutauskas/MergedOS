# Networking (M5 slice): spec

Written before the code. Own stack from the RFCs (791, 792, 793, 826, 1071,
1122, 2131, 6298, 5681-style loss recovery); no code taken from other
projects. This supersedes the roadmap's "port the FreeBSD stack".

## Layout

- `kernel/net/net.h` - public API of the portable core; `netcore.c`
  (checksum, Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP client, loopback) and
  `tcp.c` are *portable*: no kernel calls except the allocator, so they are
  linked into the host unit tests too. State lives in one `struct nstack`
  (an instance, not globals), so a test can connect two stacks back to back
  through a lossy virtual wire.
- `kernel/net/virtio_net.c`, `kernel/net/e1000.c` - NIC drivers (polled).
- `kernel/net/netdev.c` - NIC probing, the `netd` kernel thread (polls the
  NIC and runs timers every tick), resolv.conf, boot self-test.
- `kernel/net/sockets.c` - socket files, `inet_socket_ops`, and the
  temporary AF_INET-only Linux dispatch (`kernel/net/sock_linux.c`).

## Stack behaviour

- Ethernet II, ARP (16-entry cache, one queued packet per unresolved entry,
  retried), IPv4 (no fragment reassembly: fragments are dropped; no
  fragmentation on send: EMSGSIZE), ICMP echo reply + echo request API
  (`ns_ping`), UDP (bounded per-socket queue), loopback 127.0.0.0/8 and
  own address delivered through a queue inside the stack.
- TCP: 3-way handshake, MSS option, sliding window (16-bit, no window
  scaling), cumulative ACK, RTO per RFC 6298 (min 200 ms, backoff, 8
  retries then ETIMEDOUT), slow start / congestion avoidance, fast
  retransmit after 3 duplicate ACKs, zero-window persist probe, Nagle
  (off with TCP_NODELAY), delayed-ACK-free (ACK every segment), FIN/RST
  handling, simple out-of-order list (16 segments), TIME_WAIT 2 s,
  listen backlog, RST for segments to closed ports.
- DHCP client (DISCOVER/OFFER/REQUEST/ACK, retransmit with backoff, lease
  renewal not implemented: the lease is taken once). On ACK the kernel
  writes `/etc/resolv.conf` (ramfs mount `/etc`) and `/etc/hosts`.
- DNS: stub resolver *configuration* only (resolv.conf from DHCP);
  user space (Wine, glibc, musl) does the queries over UDP.
- IPv6: not implemented; `socket(AF_INET6)` returns EAFNOSUPPORT.
  Later: ND, ICMPv6, dual-stack sockets.

## Sockets (Linux personality, AF_INET)

`socket`, `bind`, `listen`, `accept`/`accept4`, `connect` (blocking, and
non-blocking with EINPROGRESS then SO_ERROR), `sendto`/`recvfrom`,
`sendmsg`/`recvmsg` (single iovec batch), `shutdown`, `getsockname`,
`getpeername`, `setsockopt`/`getsockopt` (SO_REUSEADDR, SO_ERROR, SO_TYPE,
SO_BROADCAST, TCP_NODELAY, SO_RCVBUF/SNDBUF, SO_KEEPALIVE accepted),
`ioctl` FIONBIO/FIONREAD, plus `read`/`write`/`close`/`fcntl` on the
descriptor. SOCK_NONBLOCK/SOCK_CLOEXEC, MSG_DONTWAIT, MSG_PEEK.

### Seam to the shared socket layer (kernel/socket.c, another branch)

- `inet_socket_ops` (kernel/net/inet.h) is a table of methods that work on
  kernel buffers only (no user pointers): create, bind, connect, listen,
  accept, sendmsg, recvmsg, shutdown, getsockopt, setsockopt, poll (a
  readiness mask using Linux POLLIN/POLLOUT/POLLERR/POLLHUP values),
  getname, ioctl, release. A generic socket file just stores the ops pointer
  and a `void *priv` and forwards.
- Until that layer exists, `kernel/net/sock_linux.c` implements the Linux
  syscalls 41-55 and 288 for AF_INET only on a `F_SOCKET` file type
  (`struct file::sock`), hooked into the Linux table through the weak
  function `net_linux_lookup()`. When the shared layer lands, delete
  `sock_linux.c` and the `F_SOCKET` cases in vfs.c, and register
  `inet_socket_ops` for AF_INET.
- poll/epoll are not implemented here: use `inet_socket_ops.poll`.

## Tests

- Host (ASan/UBSan): checksum vectors, ARP/IP/UDP parsing, TCP between two
  stacks over a wire with loss, reordering and duplication (transfer 200 KB
  both ways, close, RST, refused connect, zero window), fuzzed/truncated
  frames into `ns_input`.
- Boot (`tools/boot-test.sh`, QEMU `-netdev user` + virtio-net-pci): DHCP
  lease, ping 10.0.2.2, DNS query to 10.0.2.3, and a static guest program
  that does an HTTP GET against a host-side `python3 -m http.server` via
  10.0.2.2. No external internet needed. e1000 gets the DHCP/ping check.

## Status

Implemented as specified, all in `make test`:

- virtio-net (legacy/transitional PCI, I/O BAR) and e1000 (82540EM family),
  both polled by the `netd` thread once per timer tick (about 10 ms of
  latency; spinning starves QEMU's timer). Both run the full boot test.
- Stack, DHCP, resolv.conf (`/etc/resolv.conf` and `/etc/hosts` are
  single-file ramfs mounts; the rest of `/etc` stays read-only), loopback,
  AF_INET sockets, host tests with a lossy two-stack wire.
- Footprint: kernel footprint 872 KiB at the end of the boot test (base 664):
  about 100 KiB of NIC rings and buffers, 16 KiB netd stack, the stack code
  and static socket tables.

Not done: IPv6, IP fragmentation/reassembly, TCP window scaling/SACK/
timestamps, delayed ACK, keep-alive probes, SO_RCVTIMEO/SNDTIMEO (accepted,
ignored), raw and ping sockets, modern-only virtio (0x1041), interrupt-driven
receive, DHCP renewal, poll/epoll (shared layer), `SO_LINGER`, MSG_OOB,
fstat reports sockets as FIFOs, ISNs are not cryptographically random.
