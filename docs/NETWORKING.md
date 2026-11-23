# Networking, servers and remote desktop

Network cards, Wi-Fi, the SSH and web servers, RDP. Back to the [README](../README.md).

## Networking

IPv6 is optional and **off by default**. Settings > Network and `toggle6`
share the same saved switch; `toggle6 on` / `toggle6 off` set it explicitly.
With no argument, `toggle6` flips it. `ipv6` displays the current addresses,
router and DNS. Disabling IPv6 drops its queued packets and ends IPv6 TCP
sessions while IPv4 continues operating.

After rebuilding, test directly inside Banana OS:

```sh
selftest ipv6                  # no router/Internet needed; restores the toggle
ipv6                          # confirm its original on/off state
```

To test a real IPv6 network, enable it and wait for the router's SLAAC prefix:

```sh
toggle6 on
ipv6
nslookup -6 example.com
ping6 -c 2 example.com
curl -6 http://example.com/
selftest ipv6 example.com 80
```

HTTP/HTTPS clients (including Browser and SDK apps) accept bracketed IPv6
literals. By default they try IPv4, then IPv6 if enabled; `curl -4` / `-6`
and `wget -4` / `-6` force a family. Existing server commands still listen on
IPv4; the TCP layer exposes a separate `tcp_listen6()` API.

Current limits: no IPv6 fragment reassembly, source fragmentation, or path-MTU
discovery. TCP uses a 1220-byte MSS and UDP allows 1232 data bytes so packets
fit the 1280-byte IPv6 minimum MTU. AAAA DNS uses an advertised IPv6 resolver
when available, or the configured IPv4 DNS server. DNS replies truncated for
TCP fallback are not supported. Local tests exercise packet validation,
checksums, transport data/close, and toggle restoration; optional host tests
require an actual IPv6 route.


Banana OS brings up the network card by itself at boot and configures it with **DHCP** in the background - by the time you're at the prompt, `ifconfig` usually already shows an address.

```
ifconfig                              # interface, address, gateway, DNS, packet counters
ping -c 3 example.com                 # ICMP echo (Ctrl+C stops)
nslookup github.com                   # DNS lookup
curl http://example.com               # print a page
curl -L -o page.html https://github.com  # follow redirects, save to a file
curl -I https://example.com           # headers only
curl -v https://example.com           # show the connection, TLS cipher, request and response headers
wget https://raw.githubusercontent.com/nothings/stb/master/README.md   # download a file
netstat                               # TCP connections
arp                                   # ARP cache
dhcp                                  # back to DHCP / renew the lease
ifconfig eth0 192.168.1.50 netmask 255.255.255.0 gw 192.168.1.1 dns 1.1.1.1   # static setup
ifconfig reset                        # forget the saved settings
```

**The configuration is permanent:** `ifconfig <if> <ip> ...` (static), `ifconfig <if> up` (choose the interface) and `dhcp` save it in `/etc/network.conf`, and it is applied again at every boot - also when the saved interface is a USB adapter plugged in later. On an installed system (`install`, below) the file is written to the disk immediately; on the live CD it only lasts until you power off.

What's implemented, all from scratch:

| Layer | |
|---|---|
| NIC drivers | Intel 8254x "e1000" (DMA descriptor rings, IRQ wakeups), AMD PCnet (32-bit descriptor rings), virtio-net (legacy virtqueues), Realtek RTL8139, Realtek RTL8111/8168 (C+ descriptor rings), USB: Realtek RTL8152/8152B, CDC-ECM |
| Link | Ethernet II, ARP (cache, request queueing), loopback |
| Network | IPv4 (routing via default gateway, DF, no fragmentation), ICMP echo request/reply |
| Transport | UDP, TCP (client and server: 3-way handshake both ways with a listen backlog, sliding window, RTT-estimated retransmission timeout with backoff, fast retransmit, slow start/congestion avoidance, out-of-order reassembly, delayed ACKs, zero-window probing, orderly close) |
| Services | DHCP client (with lease renewal), DNS resolver (with CNAMEs + cache) |
| Security | TLS 1.3 client: X25519, TLS_AES_128_GCM_SHA256, TLS_CHACHA20_POLY1305_SHA256 |
| Application | HTTP/1.1 client: Content-Length, chunked encoding, redirects; HTTP server; SSH-2 server (Ed25519, curve25519, ChaCha20-Poly1305, AES-128-GCM) |

> ⚠️ **HTTPS note:** TLS encrypts and authenticates the whole session (the handshake transcript and Finished MACs are verified), but Banana OS ships no certificate authority store, so it does **not** verify the server's certificate - like `curl --insecure`. That protects against eavesdropping, not against an active man-in-the-middle. Don't use it for anything sensitive.

### QEMU

```bash
make run            # e1000 NIC on QEMU user-mode (NAT) networking, serial console on this terminal
make run-rtl8139    # same, with the RTL8139 card
# other cards: -nic user,model=pcnet  or  -nic user,model=virtio-net-pci
make run-headless   # no window: use Banana OS entirely through the serial console
```

or by hand:

```bash
qemu-system-i386 -cdrom Banana_OS.iso -m 256 -nic user,model=e1000
```

QEMU's user-mode networking gives the guest `10.0.2.15`, a gateway at `10.0.2.2` and DNS at `10.0.2.3` via DHCP, and NATs TCP/UDP to the real Internet. For **ping** to reach Internet hosts, QEMU forwards ICMP with unprivileged "ping sockets", which a Linux host has to allow (pinging `10.0.2.2` always works):

```bash
sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"
# in an unprivileged LXC container the range must fit the container's ID map:  "0 65535"
```

For the guest to sit directly on your LAN instead of behind NAT, bridge it with a tap device (`make run-tap` expects `tap0` to exist and be part of a bridge).

### VirtualBox

Use the default network adapter: **Settings → Network → Adapter 1 → Attached to: NAT** (or *Bridged Adapter* to join your LAN), adapter type **Intel PRO/1000 MT Desktop (82540EM)**. The *Intel PRO/1000 T Server* and *MT Server* types work too; the PCnet and virtio adapters are not supported.

## Servers: SSH and web

### SSH (`sshd`)

```
passwd                 # choose the password of `banana` - it is the SSH login
sshd start             # listen on port 22 (sshd start 2222 for another port)
sshd enable            # ...and start it at every boot
sshd                   # status: address to use, host key fingerprint, open sessions
```

Then, from another computer: `ssh banana@<address shown by ifconfig>`. You get a normal Banana OS shell - `ls`, `cat`, `ping`, `curl`, `wget`, `httpd`, ... and `exit` to log out. A single command works too: `ssh banana@10.0.2.15 "ls /etc; uname"`.

- Protocol: SSH-2 written from the RFCs - curve25519-sha256 key exchange (with the "strict KEX" fix against the Terrapin attack), an Ed25519 host key (created on first start, kept in `/etc/ssh`), chacha20-poly1305@openssh.com or aes128-gcm@openssh.com. Works with OpenSSH, PuTTY and Windows' `ssh`.
- The first connection shows the host key fingerprint (`SHA256:...`): compare it with what `sshd` prints on the Banana OS screen before answering "yes".
- Password login only (no keys yet); the password is stored salted and stretched (PBKDF2-HMAC-SHA256, 10000 rounds) in `/etc/shadow`. 5 wrong passwords close the connection, each costs 1.5 s. `passwd -d` removes it (SSH logins refused).
- Up to 2 sessions at the same time. The full-screen editor (`edit`) only runs on the local screen - over SSH write files with `echo "text" > file` / `>>`.
- No SFTP/scp or port forwarding.

### Web server (`httpd`)

```
httpd start            # port 80 (httpd start 8080 for another port)
httpd enable           # ...and at every boot
httpd                  # status and the last requests
echo "<h1>Hello</h1>" > /var/www/index.html
wget -O /var/www/photo.jpg https://...
```

Open `http://<address>/` in a browser. Files under `/var/www` are served (GET/HEAD) with their content type; a folder without `index.html` gets a file listing. A new, empty `/var/www` gets a welcome page and `demo.php`.

**PHP**: a `.php` file is run on each request (as is `index.php` for a folder) and its output is the page. It is Banana OS's own interpreter for a basic PHP: variables, arrays (ordered maps), functions, `if/for/foreach/while/switch`, `try/catch`, `<?= ?>` and the `endif;`/`endforeach;` forms, string interpolation, and about 90 functions (strings, arrays and sorting, math, `date()`, `json_encode/decode`, `md5/sha1`, `htmlspecialchars`, `sprintf`, `print_r/var_dump`, ...). `$_GET`, `$_REQUEST` and `$_SERVER` are set from the request; `header()` sets the content type, the status or a redirect; `file_get_contents`, `file_put_contents` (with `FILE_APPEND`), `file`, `file_exists`, `unlink` and `scandir` work on the Banana OS files (relative to the script's folder). No classes, includes, sessions or POST bodies; errors are shown in the page, like `display_errors=On`.

For example, `edit /var/www/hello.php`, then open `http://<address>/hello.php?name=Ann`:

```php
<h1>Hello <?= htmlspecialchars($_GET['name'] ?? 'world') ?></h1>
<?php foreach ([1, 2, 3] as $n) echo "<p>$n squared is " . $n * $n . "</p>"; ?>
```

**On the internet**: `httpd expose [port]` asks your router, over UPnP, to forward a public port (80 by default) to Banana OS's web server, and prints the public address (`http://<public ip>:<port>/`); `httpd unexpose` removes the forwarding. It needs a router with UPnP turned on and Banana OS on that router's LAN (bridged networking in a VM); behind QEMU's NAT or a carrier-grade NAT it says so. Anything in `/var/www` is then readable by anyone, and `.php` pages run for them.

In **QEMU's user-mode network** (the default `make run`), the guest is behind QEMU's NAT, so forward ports to reach the servers from your computer:

```bash
qemu-system-i386 -cdrom Banana_OS.iso -m 256 -nic user,model=e1000,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80
ssh -p 2222 banana@localhost        # and http://localhost:8080/
```

With a bridged network (VirtualBox "Bridged Adapter", QEMU tap) the machine has its own address on your LAN - no forwarding needed.

## Remote Desktop (RDP)

Banana OS speaks the Remote Desktop Protocol (MS-RDPBCGR) both ways, over its own TLS 1.2. Both halves are off by default and are turned on from *Settings > Startup* or the shell, like `sshd`.

### Server (`rdpd`) - connect *to* Banana OS

```
rdpd start             # listen on port 3389 (rdpd start 4000 for another port)
rdpd enable            # ...and start it at every boot
rdpd                   # status, certificate fingerprint, the open session
rdpd stop
```

Then from another machine open a Remote Desktop client and connect to Banana OS's address - Windows' **Remote Desktop Connection** (`mstsc`), the **Microsoft Remote Desktop** apps (Mac, iOS, Android), or **FreeRDP** (`xfreerdp /v:<address> /u:banana`). You see the Banana OS desktop; your keyboard and mouse drive it.

- **Log in** as `banana` with the password you set with `passwd` (the same one SSH uses). Ten wrong tries from an address lock it out for fifteen minutes, as with SSH.
- **Transport is TLS** (the X.224 negotiation asks for it); a client that insists on NLA only is refused. On the first start an RSA-2048 key and a self-signed certificate are generated (it says so - it takes a moment) and kept in `/etc/rdp/`. `mstsc` warns once about the self-signed certificate; its fingerprint is what `rdpd` prints.
- The screen is sent as changed 64x64 tiles (32-bit colour); one client at a time.

### Client (`rdp`) - connect *from* Banana OS

```
rdp 192.168.1.50            # asks for the password (blank = none), opens a window
rdp -u admin server.lan     # a user name
rdp -u admin -d WORK host:3390
```

`rdp` opens a window showing the other computer's desktop; while it has the focus your keyboard (exact scancodes) and mouse go to it. It connects to Windows, **xrdp**, **FreeRDP** or another Banana OS. When a password is given it uses **NLA** (CredSSP with NTLMv2); otherwise the login travels inside the TLS connection. Bitmaps arrive uncompressed, as RDP 6.0 planar, or interleaved-RLE. Run it from the desktop (`startx`), since it opens a window.

Over QEMU's NAT, the host is `10.0.2.2` from inside Banana OS, so `rdp 10.0.2.2` reaches an RDP server on your computer.
