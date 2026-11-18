# Dell iDRAC6 Virtual Console (Avocent AVSP/APCP) - protocol specification

Target: iDRAC6 fw 2.92, Dell PowerEdge R210 II "the-bmc", <IDRAC_IP> (2026-11-18).
Source: avctKVM.jar from https://<IDRAC_IP>/software/, decompiled with CFR 0.152
(`viewer/src`), string tables decoded by `tools/DumpZ.java` and inlined into
`viewer/src_annot` (same tree, obfuscated strings replaced by `/*"..."*/`, decoder
static blocks stripped). Read `src_annot`, not `src`.

Legend: **VERIFIED** = observed on the wire against <IDRAC_IP> by `probe_kvm.py`
(logs: `captures/probe1.log`, `captures/probe2.log`; pcaps `captures/probe*.pcap`;
raw video packets `captures/probe2_pkts/`). **FROM-CODE** = read from the decompiled
Java, class/method cited (package `com.avocent` abbreviated to `c.a.`).

All multi-byte integers on the KVM channels are **big-endian**.

---------------------------------------------------------------------------------

## 0. TL;DR for the implementer

1. (optional) HTTPS login -> fetch `viewer.jnlp(...)` -> one-time `user`/`passwd`.
   Simpler: **the KVM port accepts the real iDRAC username/password directly**
   (VERIFIED: `root`/`<PASSWORD>` in the LoginRequest -> result 0). Web step can be skipped.
2. TCP 5900 #1 (control): send 53-byte `APCP` hello (chan type 3), read 53-byte
   reply; reply caps=0x4 -> wrap socket in TLS (TLS1.2 AES256-GCM works). Send
   LoginRequest (`BEEF` 0x0102, 217 bytes). Read LoginResponse (0x8305), byte0==0 = OK.
3. TCP 5900 #2 (video): `APCP` hello (chan type 4); reply caps=0x1 -> **plain TCP**.
   Send VideoChannelAuth (16 bytes: `00000000 01 01 0010` + u32 client_rand + u32 key).
4. Server sends VideoConnectStatus (video, type 0x84) -> client sends on control:
   VideoEnable 0x030E, SetDisplayArea 0x0302 (1024,768), SetScaleMode1to1 0x0304.
5. Video arrives on #2 as type 0x81 (15-bit DVC) packets, <=8192 bytes each, 12-byte
   sub-header (`deadbeef`, height, width, flags BOF/EOF). Concatenate BOF..EOF and run
   the DVC decoder (section 4: 5 opcodes, ~60 lines). Ack every 20 video packets.
6. Every 2 s on control: KeepAlive 0x0400 (+ GetAvailableServers 0x0420).
7. Keyboard: 0x0200 with USB HID usage + press/release. Mouse: 0x0201 absolute pixel
   x/y + button mask + wheel.

Decoder complexity: low. The whole 15-bit decoder is a byte-oriented RLE/copy scheme;
a straight Python port decodes a full 1024x768 frame (187 KB compressed) in 0.3 s,
incremental updates (4-60 bytes) in microseconds of real work. JS/Go port is trivial.

---------------------------------------------------------------------------------

## 1. Session bootstrap from the web UI

### 1.1 Login - VERIFIED
- `GET https://<IDRAC_IP>/login.html` -> sets cookie `_appwebSessionId_=<32 hex>`
  (`Set-Cookie: _appwebSessionId_=...; path=/; secure`). Server: `Mbedthis-Appweb/2.4.2`.
- `POST /data/login`, body `user=root&password=<PASSWORD>`
  (`application/x-www-form-urlencoded`; JS `sendLoginRequest()` in login.html), cookie above.
  Reply (text/xml):
  ```
  <root> <status>ok</status> <authResult>0</authResult>
  <forwardUrl>index.html?ST1=4cb7a8aa54a136bb3a413cd6279bf4a2,ST2=3aebdebc4a9b66924a394a85699ff30e</forwardUrl> </root>
  ```
  authResult != 0 = failure (index into errMsg[] in login.html).
  ST1/ST2 are CSRF-style tokens; ST1 is used in the JNLP URL, ST2 as header on data calls.
- TLS: legacy. curl needs `--ciphers DEFAULT:@SECLEVEL=0`; Python `ctx.set_ciphers('DEFAULT:@SECLEVEL=0')`.

### 1.2 Viewer launch URL - VERIFIED
From `sysSummary.jsesp` `QuicklaunchKVM()` (Java plugin branch, `launchType == 2`):
```
GET /viewer.jnlp(<ip>@<ipv6flag>@<title>@<cache-ms>@ST1=<ST1>)
    ipv6flag = 0 for IPv4; title = urlencoded "<dns or [ip]>, <sysname>, User:<user>"
    cache-ms = Date.getTime(); requires the _appwebSessionId_ cookie
```
Example (VERIFIED, 200, `application/x-java-jnlp-file`):
`/viewer.jnlp(<IDRAC_IP>@0@the-bmc%2C%20User%3Aroot@1795001373008@ST1=4cb7...)`
Without a valid cookie: empty reply; with bad ST1: 401.
VM-only variant: `/data?type=jnlp&get=vmStart(...)`. ActiveX variant:
`activeXViewer.esp?ipAddr=..&kvmPort=..&title=..&TokenName=..&TokenKey=..` (not used).

### 1.3 JNLP content - VERIFIED
```
main-class com.avocent.idrac.kvm.Main
<argument>ip=<IDRAC_IP></argument>
<argument>vmprivilege=true</argument>
<argument>helpurl=https://<IDRAC_IP>:443/help/contents.html</argument>
<argument>title=the-bmc%2C%20User%3Aroot</argument>
<argument>user=1804289383</argument>
<argument>passwd=846930886</argument>
<argument>kmport=5900</argument>
<argument>vport=5900</argument>
<argument>apcp=1</argument>
<argument>version=2</argument>
jar: https://<IDRAC_IP>:443/software/avctKVM.jar (+ avctKVMIO<os>.jar, avctVM<os>.jar nativelibs)
```
- `user`/`passwd` are a one-time KVM credential pair. **Observation (VERIFIED):**
  successive JNLP fetches returned 1804289383/846930886, 1681692777/1714636915,
  1957747793/424238335, 719885386/1649760492 ... = the glibc `rand()` sequence with the
  default seed 1 (values #1..#8 since the iDRAC booted). The tokens are therefore
  predictable - a security weakness worth noting, not needed by us.
- Token lifetime (VERIFIED): after `/data/logout` of the web session the token is
  rejected (LoginResponse result=3). Re-use while the web session is alive was
  not tested separately (the probe uses each token once).
- **Direct login (VERIFIED):** LoginRequest with username `root`, password
  `<PASSWORD>` -> result 0. Garbage credentials -> result 3. So a client may skip HTTPS
  entirely and authenticate on 5900 with real iDRAC credentials.

### 1.4 Argument -> property mapping - FROM-CODE
`c.a.a.n.a` (String[][] static, dumped with `tools/DumpA.java`):
```
title->TITLE  user->USERNAME  passwd->PASSWORD  vport->VPORT  kmport->KMPORT
helpurl->HELP_URL  ip->HOST  apcp->USE_APCP  sslv3->SSLV3  serverkey->SERVER_KEY
rip->RIP  port->PORT  channel->CHANNEL  debug->DEBUG  F2->chat  saveSession
sessionID->SESSIONID  csrfToken->CSRFTOKEN
```
iDRAC extras (`c.idrac.kvm.f`): url->dvcurl, vmprivilege, length, vfolder->F1.
Defaults (`c.a.a.j`, `c.idrac.kvm.d.F()`): VPORT 2068, KMPORT 8192, PORT 1, CHANNEL 0,
USE_APCP false, SSLV3 false. `version=2` is not mapped (ignored).
`c.kvm.b.r.c(int)` sets P = KMPORT (control), `d(int)` sets Q = VPORT (video).

### 1.5 Logout - VERIFIED
`GET /data/logout` with the cookie (+ header `ST2: <ST2>`) -> 302. `racadm getssninfo`
afterwards showed no leftover GUI/KVM sessions.

---------------------------------------------------------------------------------

## 2. Connections, APCP pre-handshake, TLS, authentication

Two TCP connections, both to 5900 here (KMPORT == VPORT):
- **control** ("Primary", `c.kvm.b.r.G`, reader thread `y` "AVSP Session Receive")
- **video** (`c.kvm.b.r.H`, reader thread `z` "AVSP Video Input")

### 2.1 APCP hello (client -> server), 53 bytes - VERIFIED (FROM-CODE `c.kvm.b.l.a(byte,byte[],int)`)
```
off len
 0   4  "APCP"
 4   4  u32 total length = 53
 8   2  u16 0x0100  (message type: connect request)
10   2  u16 0
12   1  channel type: 3 = control/KVM session (l.a()), 4 = video (l.b())
13   1  client protocol major = 2
14   1  client protocol minor = 34
15   1  0
16   4  u32 requested capabilities = 5 (bit0 plain, bit2 SSL)   (r.mb = 5)
20   1  random length = 32
21  32  random bytes (java.util.Random; unused by server as far as seen)
```

### 2.2 APCP reply (server -> client), 53 bytes - VERIFIED (FROM-CODE `c.kvm.b.l.a(Socket)`)
```
 0   4  "APCP"
 4   4  u32 length (53)
 8   2  u16 0x8100 (must be 0x8100, else "Incorrect message type")
10   2  u16 (0)
12   1  server major   (observed 1)
13   1  server minor   (observed 0)
14   4  u32 capabilities: bit0 (0x1) = plain TCP, bit2 (0x4) = wrap in SSL/TLS
18   2  u16 redirect port: if != 0 client closes and reconnects to that port (observed 0)
20   1  random length (observed 0)
21  32  random (zeros)
```
Observed raw control reply:
`41504350 00000035 8100 0000 01 00 00000004 0000 00 00..00` -> caps=0x4 (TLS).
Observed raw video reply: same but caps `00000001` -> **plain TCP**.
Client logic: caps&1 -> plain; else caps&4 -> SSL; else error. If server version <
2.34 the client downgrades its own idea of the version to 2.33 (`r.bb/r.cb`) - no
visible effect on message formats (LoginRequest is still the 0x0102 variant).

**Note:** `cfgRacTuneConRedirEncryptEnable=0` did NOT turn off TLS on the control
channel; it only matters for the video channel (plain).

### 2.3 TLS - VERIFIED
After the 53-byte reply the *same* TCP socket is wrapped (`SSLSocketFactory.createSocket(sock,
host, port, true)`), client initiates. No client cert, server cert not validated (trust-all
TrustManager `c.kvm.b.m` / `c.kvm.b.c`). The Java client enables only ancient suites
(SSL_RSA_WITH_RC4_128_MD5, 3DES, DH_anon...), but the server negotiates
**TLSv1.2 AES256-GCM-SHA384** with a modern OpenSSL client (Python `ssl`, ciphers
`ALL:@SECLEVEL=0`, CERT_NONE). Server cert is 1245 bytes DER.

### 2.4 Common message header ("BEEF" framing)
```
off len
 0   4  magic: "BEEF" (0x42454546) for control-channel messages
 4   2  u16 message type
 6   2  u16 total length INCLUDING this 8-byte header
 8   n  body (length-8 bytes)
```
FROM-CODE `c.kvm.b.a.b.a(int,int)` builds the header; `c.kvm.b.a.a.a(DataInputStream,..)` reads.
Reader rules (FROM-CODE): length must be >= 16 and <= 15000, i.e. every message has at
least an 8-byte body (short messages are zero-padded to 16 total) - VERIFIED: all
fixed messages seen are 16 bytes. On the **video** socket the reader uses only header
byte 5 as the type (high byte ignored) and ignores bytes 0..3:
VERIFIED video headers: `00000000 0084 0010`, `00000000 0081 2000`, and once
`42454546 8203 0010` (BEEF on the video socket). So server->client video types are
effectively 0x80..0x8A; the code also accepts 0x86xx aliases (0x8601 == 0x81 etc.).

### 2.5 LoginRequest (control, C->S), type 0x0102, 217 bytes - VERIFIED
FROM-CODE `c.kvm.b.a.ab` (constructed in `r.a(int,int,boolean)`; type 258 when proto>=2, else 256 w/o last byte).
```
 8   1  username length
 9  96  username, UTF-8, NUL padded            (JNLP 'user' token, or real username)
105  1  password length
106 96  password, UTF-8, NUL padded            (JNLP 'passwd' token, or real password)
202  8  RIP id (8 bytes from hex 'rip' arg; zeros if none)
210  1  PORT    (default 1)                    (ab.o via d(int))
211  1  CHANNEL (default 0)                    (ab.p via c(int))
212  4  u32 client random C = (int)(Math.random()*1e7)   - reused in VideoChannelAuth
216  1  share mode: 0 normal, 1 = join shared session (SHARED_SESSION); valid 0,1,2,5,6,7
```
VERIFIED header: `42454546 0102 00d9`.

### 2.6 LoginResponse (control, S->C), type 0x8305, 115 bytes - VERIFIED
FROM-CODE `c.kvm.b.a.vb(true)` + handler `c.kvm.b.n.a(vb)`.
```
 8   1  result: 0 ok; 1,2 BAD_LOGIN; 3 ACCESS_DENIED; 4,12 IN_USE (offer shared);
          8 ALL_CHANNELS_IN_USE; 15 SHARING_DENIED; 16 SHARING_TIMEOUT; 24 LICENCE_EXPIRED;
          other -> BAD_LOGIN
 9   1  (vb.k)  observed 0x23 = 35
10   1  (vb.l)  observed 0x1e = 30   (r.U "user timeout"? default 30)
11   1  unused (0)
12   4  u32 video session key D  -> echoed in VideoChannelAuth (observed 0)
16   2  u16 flags: bit0 = video on same connection (skip 2nd socket), bit1 = ?,
          bit5 = sharing-capable, bit6 = view-only. Observed 0x0010.
18   1  name length (observed 0)
19  96  name (user who holds the session, for IN_USE)
```
Observed body: `00 23 1e 00 00000000 0010 00 ...` (result 0). Bad token: `03 23 1e 00 ...`.
On result 0 and flags bit0 == 0 the client opens the video connection (`r.z()`), then
starts the keep-alive thread (`r.x()`).

### 2.7 Video channel open + VideoChannelAuth - VERIFIED
`r.z()`: new TCP to VPORT, APCP hello with channel type 4 (`l.b()`), reply caps 0x1 ->
plain. Video socket SO_TIMEOUT 60 s (property `video_socket_timeout`). Then send
(FROM-CODE `c.kvm.b.a.ac`, header built by `c.kvm.b.a.zb.a()` - note NOT "BEEF"):
```
00 00 00 00  01  01  00 10   u32 C (client random from LoginRequest)   u32 D (key from LoginResponse)
```
VERIFIED: `0000000001010010 007dbd38 00000000`.

### 2.8 Start of video - VERIFIED
Immediately after VideoChannelAuth the server sends, in this order (probe2.log):
```
ctrl  0x8302 Protocol Version   body 02 10            (rb: major 2, minor 16)
ctrl  0x8102 MouseAccelResponse body 01               (ob -> property mouse_accel=1)
ctrl  0x8420 User Privilege     body 01 00 04 01 ff   (wb: u16 @3 = 0x01ff; bit 0x10 -> power menu allowed)
video 0x84   Video Connect Status body 00             (xb: byte0==0 -> connected) -> client r.y()
video 0x8203 Scaling (tb, no payload used)
ctrl  0x8206 Input Resolution   body 0400 0300 0001   (lb: 1024 x 768)
ctrl  0x8200 Input Resolution   body 0400 0300        (lb)
ctrl  0x8201 Display Resolution body 0400 0300        (jb: 1024 x 768 -> client sends ScreenRefresh)
ctrl  0x8204 DVCColorModeResponse body 0f 01 00       (l: depth 15, colour=1) (reply to 0x0403 query)
video 0x81   DVC video ... (first full frame ~350 ms later, 23 packets / 187 KB)
ctrl  0x8430 AvailableServerNames (every 2 s, reply to GetAvailableServers)
```
`r.y()` (on 0x84) sends on **control**: VideoEnable(true) `BEEF 030E 0010 01 01 00..`,
SetDisplayArea `BEEF 0302 0010 0400 0300 0000 0000` (always 1024x768 constant in code),
SetScaleMode1to1 `BEEF 0304 0010 00..`. VERIFIED that the server streams after these.
On the first video packet of a new type the client sends SetDVCColorDepth with byte0=1
(`n.a(hb)` -> `new k().b(false)`) which the server answers with 0x8204 (VERIFIED).

---------------------------------------------------------------------------------

## 3. Message type tables

### 3.1 Client -> server (control socket unless noted)

| type | name (class) | body (8 bytes unless noted) | status |
|------|--------------|------------------------------|--------|
| 0x0102 | LoginRequest (`b.a.ab`) | see 2.5 (209 bytes) | VERIFIED |
| 0x0000 | Video Ack (`b.a.cb`) **video socket** | [0]=count of video pkts since last ack (&0xFF) | VERIFIED sent (server effect not measured) |
| 0x0001 | VideoChannelAuth (`b.a.ac`) **video socket**, non-BEEF hdr | u32 C, u32 D | VERIFIED |
| 0x0200 | KeyboardDataRequest (`b.a.f`) | [0]=0, [1]=0 press / 1 release, [2..3]=u16 USB HID usage | FROM-CODE |
| 0x0201 | Mouse Data Request (`b.a.gc`) absolute | [0]=0,[1]=buttons,[2..3]=x,[4..5]=y,[6..7]=wheel s16 | FROM-CODE |
| 0x0202 | MouseOrigin (`b.a.h`) | zeros | FROM-CODE |
| 0x0204 | Keyboard LED Request (`b.a.g`) | zeros -> reply 0x8100 | VERIFIED |
| 0x0208 | FocusControl (`b.a.d`) | [0]=1 focus gained / 0 lost | FROM-CODE |
| 0x0209 | Mouse Delta Request (`b.a.hc`) relative | same layout as 0x0201, x/y = signed deltas | FROM-CODE |
| 0x020A | SetMouseAccel (`b.a.t`) | [0]=accel value | FROM-CODE |
| 0x0301 | ScreenRefresh (`b.a.n`) | zeros -> full frame | VERIFIED (full frame followed) |
| 0x0302 | Set Display Area (`b.a.x`) | u16 w, u16 h | VERIFIED sent |
| 0x0304 | SetScaleMode1to1 (`b.a.o`) | zeros | VERIFIED sent |
| 0x030E | Video Enable Request (`b.a.db`) | [0]=1 enable/0 disable, [1]=1 | VERIFIED sent |
| 0x0320 | "We are here" (`b.a.w`) | u16 id, u16 len, string | FROM-CODE (UI ping, optional) |
| 0x0400 | KeepAlive (`b.a.p`) | zeros, every 2 s | VERIFIED sent (no reply) |
| 0x0402 | SetVideoTransmitLimit (`b.a.u`) | [0]=0..4 bandwidth class | FROM-CODE |
| 0x0403 | SetDVCColorDepth (`b.a.k`) | [0]=1 query / 0 set, [1]=depth, [2]=1 colour 0 grey, [3]=1 when setting | VERIFIED (query form) |
| 0x0413 | SharingResponse (`b.a.fc`) | [0]=answer, [2..3]=u16 request id | FROM-CODE |
| 0x0420 | GetAvailableServers (`b.a.e`) | zeros, every 2 s when RUNNING -> 0x8430 | VERIFIED |
| 0x0424 | SetPowerState (`b.a.dc`) | u16 len, server id ("current1"), u8 state | FROM-CODE (NOT sent - production host) |
| 0x0427 | RequestCredentials (`b.a.r`) | u32 30 | FROM-CODE |

### 3.2 Server -> client

| type | name (class) | body | status |
|------|--------------|------|--------|
| 0x8305 | User Login Response (`vb(true)`) | 2.6 | VERIFIED |
| 0x8300 | User Login Response, old (`vb()`) | [0]=result,[3]=flags,[4..7]=key,[8]=len,[9..]=name | FROM-CODE |
| 0x8302 | Protocol Version (`rb`) | [0] major [1] minor (2.16 observed) | VERIFIED |
| 0x8102 | MouseAccelResponse (`ob`) | [0]=accel | VERIFIED (1) |
| 0x8101 | MouseAcknowledgeResponse (`pb`) | [0]=n acked (decrements mouse send window) | FROM-CODE |
| 0x8100 | Keyboard LED (`mb`) | [0] 5 LED bits; presumably HID order bit0 Num, bit1 Caps, bit2 Scroll | VERIFIED reply `01` (bit meaning inferred) |
| 0x8420 | User Privilege Parameters (`wb`) | u16 @3; bit 0x10 = power control allowed | VERIFIED (0x01ff) |
| 0x8200/0x8206 | Input Resolution (`lb`) | u16 w, u16 h | VERIFIED |
| 0x8201 | Display Resolution (`jb`) | u16 w, u16 h -> client sends ScreenRefresh | VERIFIED |
| 0x8202 | VideoSetupData (`yb`) | brightness/contrast/positions/priority | FROM-CODE |
| 0x8203 | Scaling (`tb`) | - (seen on video socket with BEEF hdr) | VERIFIED |
| 0x8204 | DVCColorModeResponse (`l`) | [0]=depth bits, [1]=colour, [2]=? | VERIFIED (0f 01 00) |
| 0x8212 | Locked (`nb`), 0x8213 Reserved (`sb`) | - | FROM-CODE |
| 0x8301 | User Disconnect Pending (`z`) | [0]=reason -> state CLOSING | FROM-CODE |
| 0x8304 | Pending Request Cancelled (`q`) | [0]=k,[1..2]=id | FROM-CODE |
| 0x8401 | SharingRequest (`ec`) | u16 id, u8 ?, ASCII user | FROM-CODE |
| 0x8410 | SharedUserResponse (`y`) | user list | FROM-CODE |
| 0x8430 | AvailableServerNames (`c`) | u16 count; per server: u16 len, id, u16 status (status&3 = power: 1 on), u16, u16 len, string | VERIFIED (`0001 0008 "current1" 0021 0000 00`) |
| 0x8431 | SelectedServerUpdate (`bc`) | u16 len, id | FROM-CODE |
| 0x8432 | ServerStatusUpdate (`cc`) | like one 0x8430 entry | FROM-CODE |
| 0x8434 | RequestCredentials (`j`) | 32-byte user, 32-byte pw (NUL-term) | FROM-CODE |
| 0x80 | Video Packet (`fb`), not decoded - only counted for Video Ack | - | FROM-CODE |
| 0x81/0x8601 | DVC video 15-bit (`hb` -> `d.c`) | section 4 | VERIFIED |
| 0x82/0x8602 | DVC video 7-bit palette (`hb` -> `d.f`) | section 4 | FROM-CODE |
| 0x83/0x8603 | DVC video 7-bit grey (`hb` -> `d.e`) | section 4 | FROM-CODE |
| 0x8A/0x860A | DVC video 23-bit (`hb` -> `d.d`) | section 4 | FROM-CODE |
| 0x84 | Video Connect Status (`xb`) | [0]==0 connected | VERIFIED |
| 0x85/0x8605 | VideoStopped (`v`) | [0]=reason -> property video_state | FROM-CODE |
| 0x86/0x8606 | ASpeed JPEG video (`gb` -> `c.kvm.a.a.a`) | not used by this iDRAC6 | FROM-CODE |
| 0x87/0x8607 | Text Mode Video (`ub` -> `c.kvm.b.t`) | section 4.7 | FROM-CODE |
| 0x88/0x8608 | Color Palette (`ib`) | section 4.7 | FROM-CODE |
| 0x89/0x8609 | Font Table (`kb`) | section 4.7 | FROM-CODE |

Unknown types are logged and ignored (`b.a.a.a(int)` default -> `qb` NoOp).

---------------------------------------------------------------------------------

## 4. Video

### 4.1 Transport - VERIFIED
- Video packets come on the video socket, max 8192 bytes total (8-byte header +
  12-byte DVC sub-header + <=8172 data bytes).
- Rate: idle screen produced ~25 packets/s of tiny "no change" frames (4 data bytes)
  plus a cursor-blink update (50/60 bytes) every ~0.4 s; 343 video packets / 385 KB in
  22 s including two full frames.
- Client counts every decodable video packet (`r.B()`, called from `n.a(hb|fb|ub|ib|v)`);
  every W=20 packets it sends Video Ack (0x0000, count in byte 0) on the **video**
  socket (`r.Y`). If a previous ack is still queued the count is added to it.

### 4.2 DVC video packet (types 0x81/0x82/0x83/0x8A) - VERIFIED layout
FROM-CODE `c.kvm.b.a.hb.a(byte[],byte[],int)`. Offsets relative to body (after 8-byte header):
```
 0  4  ? (always de ad be ef observed)
 4  2  u16 height            (hb.l, hb.i())   observed 0x0300 = 768
 6  2  u16 width             (hb.m, hb.j())   observed 0x0400 = 1024
 8  1  flags: bit0 BOF (start of frame; reset pixel cursor to 0)
              bit1 EOF (last packet of this frame; checksum present)
              bit2 seen on all multi-packet frame packets (0x05,0x04,0x06), not used by Java
 9  1  0
10  2  u16 checksum when EOF (observed always 0 -> server does not fill it)
12  .. compressed data (packet length - 20 bytes)
```
Observed flag histogram over 343 packets: 0x05 (BOF, first of big frame) 2,
0x04 (middle) 42, 0x06 (EOF of big frame) 2, 0x03 (single-packet frame) 297.

Resolution change: decoder compares (width,height) of each packet to the framebuffer; if
different and both in 1..4096, the framebuffer is reallocated (black) and resized
(`d.b.a(ib,boolean,boolean)` -> `c.kvm.c.j.a(int,int)`, sizes > 2000 ignored). Also the
control-channel 0x8201 Display Resolution message announces it (client then requests
ScreenRefresh). There is no separate "mode change" message for DVC graphics.

### 4.3 Frame state machine - FROM-CODE (`c.kvm.b.s.f()`, `c.kvm.d.b.d()`)
- Packets before the first BOF are skipped (`s.f()`: `if (!z && !C) { f(); return; }`).
- On BOF the pixel cursor `pos` is reset to 0 (`x.f()` -> `hb.c(0)`).
- The decoder is a stream: an opcode, its operand bytes, run-length continuation
  bytes, or Make-Series continuation bytes may straddle packet boundaries; when the
  current packet is exhausted the decoder pulls the next packet (`this.b.f()`), unless
  the current packet has EOF. Implementation advice (VERIFIED working): concatenate data
  of BOF..EOF packets, decode as one buffer.
- The framebuffer is persistent across frames (ops reference previous content).
- After EOF, if checksum flag set, Java computes a checksum and on mismatch optionally
  requests ScreenRefresh (`s.r()`, property `referenceScreenOnChecksumError`). Since the
  server sends 0, ignore.

### 4.4 DVC opcode decoder - FROM-CODE (`c.kvm.d.b.d()`), VERIFIED by rendering
State: `fb[w*h]` (0xRRGGBB ints, linear row-major), `pos` (cursor), `W` (width).
Read opcode byte `op`; class = top 3 bits:
```
op & 0xE0 == 0x00  NO CHANGE    n = RUN(op);  pos += n
op & 0xE0 == 0x40  COPY ABOVE   n = RUN(op);  src = pos - W;
                                  if src >= 0: for k in 0..n-1: fb[pos+k] = fb[src+k]; pos += n
                                  else: (first line) nothing copied AND pos NOT advanced
op & 0xE0 == 0x20  COPY LEFT    n = RUN(op);  c = (pos>=1 ? fb[pos-1] : 0); fill n pixels with c
op & 0xE0 == 0x60  MAKE SERIES  (2-colour bitmap, see below)
op & 0x80 != 0     MAKE PIXEL   colour from (op & 0x7F) + 0/1/2 extra bytes (depth), fb[pos++] = colour
```
(Note: 0x60 class covers 0x60..0x7F; 0x80..0xFF are all pixels. The Java `switch`
checks 0x00, 0x40, 0x20, 0x60 then `default: if (op & 0x80) pixel`.)
Clipping (`c.kvm.c.j`): writes beyond w*h are dropped; COPY ABOVE/LEFT lengths are
clamped to the buffer end.

**RUN(op)** (`d.b.g(int)`): little-endian base-32 varint whose continuation bytes carry
the same top-3-bit class as the opcode:
```
n = op & 0x1F; k = 1
while k < 5 and next byte b has (b & 0xE0) == (op & 0xE0):
    n |= (b & 0x1F) << (5*k); consume b; k++
if mode is 7-bit (0x82 palette, 0x83 grey): n += 2       (d.e.g / d.f.g override)
```
Consequence: two consecutive runs of the same class are always merged by the encoder.
Example (VERIFIED, every idle frame): data `00 00 00 18` = NO CHANGE n = 24<<15 =
786432 = 1024*768 (whole screen unchanged).

**MAKE SERIES** (`d.b.f(int)`):
```
c0 = fb[pos-1]                          (last pixel; "n5")
c1 = first fb[k] != c0 scanning k = pos-1 down to 0; if none, c1 = c0    ("n4")
emit 4 pixels from op bits 3,2,1,0 (MSB first): bit=1 -> c1, bit=0 -> c0
if op & 0x10:                           (continuation)
    repeat: b = next byte; emit 7 pixels from b bits 6..0 (1->c1, 0->c0)
            until (b & 0x80) == 0
```
(The Java scan start is `min(pos-1, hb.g())` where `hb.g()` returns `pos` - i.e. pos-1.)

**MAKE PIXEL** per mode (pixel classes `c.kvm.d.c/d/e/f`, `b(int)`):
- 0x81 / 0x8601, 15-bit (`d.c`, "2 bytes"): `v = ((op & 0x7F) << 8) | next`;
  R = (v>>10 & 0x1F) << 3, G = (v>>5 & 0x1F) << 3, B = (v & 0x1F) << 3. (RGB555, no
  low-bit replication.) Example: `d6 b5` -> v=0x56b5 -> (0xA8,0xA8,0xA8) grey text.
- 0x8A / 0x860A, 23-bit (`d.d`, "3 bytes"): `v = ((op & 0x7F) << 16) | b1 << 8 | b2`;
  R = v>>15 & 0xFF, G = v>>7 & 0xFF, B = (v & 0x7F) << 1.
- 0x83 / 0x8603, 7-bit grey (`d.e`, "1 byte"): g = (op & 0x7F) << 1; RGB = (g,g,g). RUN +2.
- 0x82 / 0x8602, 7-bit palette (`d.f`): RGB = PAL7[op & 0x7F]; RUN +2.

PAL7 (`c.kvm.d.i.b()`, dumped with `tools/DumpPal.java`; 0xRRGGBB, index 0..127):
```
000000 000046 00007F 0000BF 004600 004646 00467F 0046BF 007F00 007F46 007F7F 007FBF 00BF00 00BF46 00BF7F 00BFBF
460000 460046 46007F 4600BF 464600 464646 46467F 4646BF 467F00 467F46 467F7F 467FBF 46BF00 46BF46 46BF7F 46BFBF
7F0000 7F0046 7F007F 7F00BF 7F4600 7F4646 7F467F 7F46BF 7F7F00 7F7F46 7F7F7F 7F7FBF 7FBF00 7FBF46 7FBF7F 7FBFBF
BF0000 BF0046 BF007F BF00BF BF4600 BF4646 BF467F BF46BF BF7F00 BF7F46 BF7F7F BF7FBF BFBF00 BFBF46 BFBF7F BFBFBF
0000FF 4600FF 7F00FF BF00FF 0046FF 4646FF 7F46FF BF46FF 007FFF 467FFF 7F7FFF BF7FFF 00BFFF 46BFFF 7FBFFF BFBFFF
00FF00 00FF46 00FF7F 00FFBF 46FF00 46FF46 46FF7F 46FFBF 7FFF00 7FFF46 7FFF7F 7FFFBF BFFF00 BFFF46 BFFF7F BFFFBF
FF0000 FF0046 FF007F FF00BF FF4600 FF4646 FF467F FF46BF FF7F00 FF7F46 FF7F7F FF7FBF FFBF00 FFBF46 FFBF7F FFBFBF
00FFFF 46FFFF 7FFFFF BFFFFF FF00FF FF46FF FF7FFF FFBFFF FFFF00 FFFF46 FFFF7F FFFFBF FFFFFF 5F5F5F 9F9F9F DFDFDF
```
(Built from a 5-level cube {0,70,127,191,255} + greys 5F/9F/DF, permuted by table `i.e`.)

Checksum (`c.kvm.c.j.a(int,int,int,int,int,int)`, used for 15-bit): sum over all w*h
pixels of `((rgb&0xFF0000)>>9 | (rgb&0xFF00)>>6 | (rgb&0xFF)>>3)` mod 65536 (black
pixels skipped). Other modes: sum of palette indices via reverse table `i.c()`.
Server sends 0 - not usable here.

### 4.5 Reference implementation
`probe_kvm.py` class `DVC` is a complete port (all four depths). Worked example from a
cursor-blink frame (VERIFIED, `captures/probe2_pkts/000051_0081.bin`):
```
data: 00 00 10 17 | 30 | 10 1f | 30 | 10 1f | 30 ...
  00 00 10 17  NO CHANGE n = 0 | 0<<5 | 16<<10 | 23<<15 = 770048   (= row 752 * 1024)
  30           COPY LEFT n = 16          (16-pixel wide cursor cell)
  10 1f        NO CHANGE n = 16 | 31<<5 = 1008 (= 1024-16, next row)
  30           COPY LEFT n = 16 ...       (repeated for each cursor scanline)
```

### 4.6 Colour depth control - FROM-CODE (`r.a(int,Object,Object)` case 3, `b.a.k`)
SetDVCColorDepth 0x0403 `[0]=0, [1]=depth, [2]=colour(1)/grey(0), [3]=1`. Depth values
from `c.kvm.c.v`: 7, 9, 12, 15, 18, 21, 23 (UI choices). The server then switches the
video packet type (0x81 15-bit, 0x8A 23-bit, 0x82 7-bit colour, 0x83 7-bit grey). Only
0x81 observed (default, DVCColorModeResponse said depth 15 colour). Not tested.

### 4.7 Text mode (VGA text, e.g. BIOS/POST if the iDRAC sends it) - FROM-CODE
Not observed (host was in a 1024x768 graphics console). Classes: `c.kvm.b.a.ub` (0x87),
`c.kvm.b.a.ib` (0x88), `c.kvm.b.a.kb` (0x89), decoder `c.kvm.b.t`, font store `c.kvm.b.k`.
- 0x87 TextModeVideo body: [4..5] u16 pixel height (y), [6..7] u16 pixel width (x),
  [8] flags: bit0 start (t), bit1 last (u), bit2 ?, bit3 blink-enable (v), bit4 line
  graphics / 9-dot (p); [9] underline scanline (q); [10] rows (w); [11] cols (x).
  If length == 12 (body 12): cursor-only "no data". If body 16 with start+last: cursor
  update: [12..13] u16 cursor offset, [14] cursor start line, [15] cursor end line.
  Otherwise [12..] = char/attribute pairs (2 bytes per cell, VGA layout) appended to a
  rows*cols*2 buffer from offset 0 on the 'start' packet; on the 'last' packet the
  final 4 bytes are cursor offset(u16)/start/end and the screen is diffed and redrawn.
  Cell size = (x/cols) x (y/rows) pixels (8 or 9 wide).
- Attribute: fg = attr & 0x0F, bg = attr >> 4 (& 7 if blink-enabled; bit7 = blink).
  Colours from the 0x88 palette (default: index 0 -> black 0xFF000000, other -> white).
  9-dot mode: column 8 duplicates column 7 for chars 0xC0..0xDF (line graphics).
- 0x88 Color Palette body: [0..1] u16 count, [2] ?, from [4]: count * (R,G,B,pad).
- 0x89 Font Table body: [0] plane (0 = font A, else B), [1] number of fonts (1/2),
  [2..3] u16 offset+1 into an 8192-byte table, [4..] glyph bytes. Font = 256 glyphs x
  32 bytes (one byte per scanline, MSB = left pixel). Applied when 8192 bytes received.
  Attribute bit3 selects font A/B in 2-font mode.

### 4.8 ASpeed JPEG (0x86) - FROM-CODE, not used here
`c.kvm.b.a.gb` + `c.kvm.a.a.*` (~85 KB of Java: JPEG-like YUV 4:2:0/4:4:4 DCT with
quantisation tables, used by Avocent firmware on AST chips). The iDRAC6 never sent it in
our tests; ignore unless a 0x86 packet is ever seen.

---------------------------------------------------------------------------------

## 5. Keyboard and mouse - FROM-CODE (not sent: production host)

### 5.1 Keyboard - `c.kvm.b.g` (lb impl), `c.kvm.b.a.f`, key listener `c.kvm.c.bb`
- keyPressed -> `f(code, true)` -> body `00 00 HH LL 00 00 00 00`
- keyReleased -> `f(code, false)` -> body `00 01 HH LL 00 00 00 00`
- Full message: `42454546 0200 0010 00 <0|1> <u16 code> 00000000` on control socket.
- `code` = **USB HID Keyboard usage ID** (page 7): A=0x04 ... Z=0x1D, 1=0x1E .. 0=0x27,
  Enter 0x28, Esc 0x29, Backspace 0x2A, Tab 0x2B, Space 0x2C, F1 0x3A .. F12 0x45,
  PrintScreen 0x46, ScrollLock 0x47, Pause 0x48, Insert 0x49, Home 0x4A, PgUp 0x4B,
  Delete 0x4C, End 0x4D, PgDn 0x4E, Right 0x4F, Left 0x50, Down 0x51, Up 0x52,
  NumLock 0x53, keypad 0x54..0x63, Application/Compose 0x65, LCtrl 0xE0, LShift 0xE1,
  LAlt 0xE2, LGUI 0xE3, RShift 0xE5, RAlt/AltGr 0xE6, RGUI 0xE7.
  Java quirk: right Ctrl is mapped to 0xE0 (not 0xE4); a new client should send 0xE4.
- Table source: `c.kvm.c.c.d` static Hashtables `i` (eng), `c` (deu), `g` (fra),
  `a` (ita), `e` (spa), `k` (?), keyed by `c.kvm.c.c.b(javaKeyCode, char, location)`;
  selected by `c.kvm.c.c.c.a(int,char,int)` from the input locale; fallback switch for
  Sun/Japanese keys (e.g. 0x75 Help, 0x76 Props, 0x7A Undo, 0x8A..0x8B, 0x87, 0x89).
  Full English dump: `tools/keymap_eng.txt` (`tools/DumpKeys.java i`).
  For an HTML5 client: map `KeyboardEvent.code` -> standard HID usage (identical values).
- The native lib (avctKVMIOLinux64.jar, `c.kvm.nativekeyboard.*`) only grabs the
  keyboard (pass-through of Alt-Tab etc.) - same wire format.
- LED state: request 0x0204 -> reply 0x8100 [0] = LED bits (VERIFIED reply `01`;
  bit order presumably HID: bit0 Num, bit1 Caps, bit2 Scroll - not confirmed).

### 5.2 Mouse - `c.kvm.b.h` (mb impl), `c.kvm.b.a.gc` / `hc`, Swing listener `c.kvm.c.db`
Absolute mode (default; Swing listener -> `h.a(...)`/`h.b(...)`/`h.a(x,y)`):
```
42454546 0201 0010  00  BB  XXXX  YYYY  WWWW
  BB   = button mask: bit0 left (Java button1), bit1 right (button3), bit2 middle (button2)
  XXXX = u16 x, YYYY = u16 y : pixel coordinates in the remote framebuffer (1:1 panel,
         scale factor h.e = 1.0); negatives clamped to 0
  WWWW = s16 wheel: -MouseWheelEvent.getWheelRotation() (up = +1) on wheel events, else 0
```
The button mask is state (`r.ab`): press ORs the bit, release clears it; each move
carries the current mask. Wheel = a "press" event with no button and WWWW != 0.
Relative mode (0x0209, `hc`, used with the native single-cursor mode): same layout, XXXX/
YYYY are signed deltas. `h.c(x,y)` (mouse entered window) first sends MouseOrigin 0x0202.
The internal event kind (1 move, 2 delta, 4 press, 8 release) is NOT on the wire; it is
used by the send queue `c.kvm.c.d.e` to coalesce consecutive moves (press/release are
never coalesced). Send window: after login the queue has no limit (-1); server ack
0x8101 [0]=n decrements the outstanding count.
Mouse acceleration: server reports 0x8102 [0] (observed 1). Client can set 0x020A [0]=v
(`r.h(int)`). The iDRAC6 web setting "mouse acceleration" (Windows/Linux/none) maps here.
Absolute vs. host cursor sync: none in protocol; host USB mouse is an absolute HID device
("Avocent USB Composite Device-0 ... USB HID v1.00 Mouse", seen in the captured frame), so
absolute coordinates land 1:1.

---------------------------------------------------------------------------------

## 6. Keepalive, power, sharing, disconnect

- KeepAlive (`c.kvm.b.x` thread "KVM Keep Alive"): every 2000 ms send 0x0400 on control;
  when SESSION_STATE == RUNNING also GetAvailableServers 0x0420. VERIFIED: server
  answers each 0x0420 with 0x8430 (power status) and never answers 0x0400. Not tested
  how long the server tolerates silence.
- Power control (`r.a(1, state)`, `b.a.dc`, type 0x0424): body `u16 len, server id from
  0x8430 ("current1"), u8 state` with 1 Power On, 2 Power Off, 3 Power Cycle, 4 Reboot
  (reset), 5 Graceful Shutdown. Requires privilege bit 0x10 in 0x8420. Status from 0x8430
  entry `status & 3` (1 = on observed; UI enables Off/Cycle/Reset/Shutdown when 1).
  Note the length field of `dc` is 8+len+3 (not padded to 16). NOT sent (production).
- Bandwidth: SetVideoTransmitLimit 0x0402 [0] 0..4 (`c.kvm.c.v` a..e).
- Sharing: if login result 4/12 (IN_USE) the UI asks and re-logs with share mode 1;
  the primary user gets 0x8401 SharingRequest (u16 id, u8 ?, user) and answers 0x0413
  SharingResponse ([0]=answer, [2..3]=id). 0x8410 lists shared users.
- Disconnect: client simply closes both sockets (`r.A()`); no logout message exists.
  Server-initiated: 0x8301 User Disconnect Pending ([0] reason) -> client state CLOSING;
  0x85 VideoStopped ([0] reason) on video. Reasons in the UI (`c.idrac.kvm.d`):
  SHUTDOWN_CHANNEL_PREEMPTED_BY_LOCAL_USER, SHUTDOWN_SESSION_IDLE_TIMEOUT_EXCEEDED,
  SHUTDOWN_PRIMARY_USER_GONE_TO_EXCLUSIVE_MODE, SHUTDOWN_LAST_ACTIVE_USER_HAS_DISCONNECTED.
  VERIFIED: closing both sockets left no session in `racadm getssninfo`.
- Video pause/resume: VideoEnable 0x030E [0]=0/1 (`r.a(6,bool)`).
- Full refresh: ScreenRefresh 0x0301 (VERIFIED: a full 187 KB frame followed, ~6 s later).

---------------------------------------------------------------------------------

## 7. Empirical validation

`probe_kvm.py` (this directory): HTTPS login -> JNLP -> APCP+TLS control login -> video
channel -> decode -> PNG (`probe_frame.png`, shows the TrueNAS kernel console at
1024x768, correct colours and layout) -> sends only read-only queries (KeyboardLED,
ScreenRefresh), keepalives and video acks -> logout. No keyboard/mouse/power traffic.

Run: `python3 probe_kvm.py [--seconds 20] [--dump dir] [--no-queries]`.
Token-semantics test: scratch `toktest.py` (results in 1.3).

Observed output (`captures/probe2.log`, abridged; repetitive 4-byte frames removed):
```
[  5.246] POST /data/login -> <root> <status>ok</status> <authResult>0</authResult> <forwardUrl>index.html?ST1=4b58...,ST2=390a...</forwardUrl>
[  5.393] GET /viewer.jnlp(<IDRAC_IP>@0@probe%2C%20User%3Aroot@1795001559858@ST1=4b58...) -> 200 args: {... 'user': ..., 'passwd': ..., 'kmport': '5900', 'vport': '5900', 'apcp': '1', 'version': '2'}
[  6.314] APCP(type=3) reply len=53 raw=4150435000000035810000000100000000040000000000...00
[  6.314]    type=0x8100 ver=1.0 caps=0x4 redirect_port=0 randlen=0
[  7.994]    TLS up: TLSv1.2 AES256-GCM-SHA384
[  7.995] C>S ctrl LoginRequest len=217 client_rand=8240440 hdr=42454546010200d9
[  9.091] S>C ctrl type=0x8305 len=115 body=0023001e000000000010000000...
[  9.091]    LoginResponse result=0 b1=35 b2=0 video_key=0x00000000 flags=0x0010 name=b''
[  9.994] APCP(type=4) reply len=53 raw=4150435000000035810000000100000000010000000000...00
[  9.994]    type=0x8100 ver=1.0 caps=0x1 redirect_port=0 randlen=0
[  9.994] C>S video VideoChannelAuth 0000000001010010007dbd3800000000
[  9.995] S>C ctrl type=0x8302 len=16 body=0210000000000000
[  9.995] S>C ctrl type=0x8102 len=16 body=0100000000000000
[  9.995] S>C ctrl type=0x8420 len=21 body=01000401ff0000000000000000
[  9.999] S>C video hdr=0000000000840010 type=0x0084 len=16 body[:40]=0000000000000000
[  9.999] C>S ctrl VideoEnable / SetDisplayArea(1024x768) / SetScaleMode1to1
[ 10.000] S>C video hdr=4245454682030010 type=0x8203 len=16 body[:40]=0000000000000000
[ 10.000] S>C ctrl type=0x8206 len=16 body=0400030000010000
[ 10.016] S>C ctrl type=0x8200 len=16 body=0400030000000000
[ 10.016] S>C ctrl type=0x8201 len=16 body=0400030000000000
[ 10.366] S>C video hdr=0000000000812000 type=0x0081 len=8192 body[:40]=deadbeef03000400050000008000232023d6b52280003123d6b522...
[ 10.366] C>S ctrl SetDVCColorDepth [01 00 00 00] (video type now 0x0081)
[ 10.366]    DVC hdr0-3=deadbeef height=768 width=1024 flags=0x05(BOF=1 EOF=0) b9=00 csum=None data=8172
[ 10.370] S>C ctrl type=0x8204 len=16 body=0f01000000000000
[ 11.470]    DVC hdr0-3=deadbeef height=768 width=1024 flags=0x06(BOF=0 EOF=1) b9=00 csum=0 data=7733
[ 11.772]    frame 1: 187517 bytes decoded in 0.30s, end pos=786432 (w*h=786432) server_csum=0 local_csum15=52542
[ 12.290] S>C ctrl type=0x8430 len=25 body=0001000863757272656e74310021000000
[ 14.016] C>S ctrl KeyboardLEDRequest (0x0204)
[ 14.049] S>C ctrl type=0x8100 len=16 body=0100000000000000
[ 18.071] C>S ctrl ScreenRefresh (0x0301)
[ 23.996]    DVC ... flags=0x05(BOF=1 EOF=0) ... data=8172
[ 24.454]    frame 179: 187517 bytes decoded in 0.38s, end pos=786432 ...
[ 32.013] --- summary (channel, type): [count, bytes]
             ctrl 0x8100 [1,16]  0x8102 [1,16]  0x8200 [1,16]  0x8201 [1,16]  0x8204 [1,16]
             ctrl 0x8206 [1,16]  0x8302 [1,16]  0x8420 [1,21]  0x8430 [10,250]
             video 0x0081 [343,385020]  0x0084 [1,16]  0x8203 [1,16]
[ 32.013] video acks sent: 17 frames completed: 299 DVC flag histogram: {5: 2, 4: 42, 6: 2, 3: 297}
[ 35.253] GET /data/logout -> 302
```
Every full frame decoded to exactly w*h = 786432 pixels (the stream is self-consistent
with the decoder; any opcode error would desynchronise the cursor).

The real Java viewer was not run under Xvfb: everything needed was confirmed by the
probe, and the jar enables SSLv3-era cipher suites that Java 21 rejects (would need an
old JRE + java.security edits).

---------------------------------------------------------------------------------

## 8. Open questions / not verified

1. Keyboard and mouse were not sent (production host). Wire format is FROM-CODE only;
   first test should be harmless (e.g. a Shift press/release, or a mouse move) when the
   host may be touched. Verify the press=0/release=1 polarity of byte 1 in 0x0200
   (code writes `j ? 0 : 1` with j=true for press - slightly unusual).
2. Whether the server throttles or stops video without Video Acks (we always acked).
3. Keepalive timeout (how long without 0x0400 before the server drops the session).
4. Text mode (0x87/0x88/0x89) never observed - need a reboot into BIOS/POST or a
   text-mode OS console to verify; format here is from `c.kvm.b.t` and may have details
   wrong (bit2 of flags, palette byte 2).
5. Other colour depths (0x82/0x83/0x8A) not exercised; SetDVCColorDepth set-form not sent.
6. Meaning of LoginResponse bytes 1/2 (0x23, 0x1e), flags 0x0010, the 0xdeadbeef DVC
   sub-header word, DVC flag bit2, and the 0x8206 extra byte (`0001`).
7. JNLP token reuse while the web session is still alive (and its expiry) not measured;
   moot if the client logs in with real credentials (VERIFIED working).
8. Multi-user sharing / IN_USE flow and server-initiated disconnect reasons not observed.
9. Server checksum is always 0 - no integrity check available; recovery = ScreenRefresh.
10. Power command 0x0424 untested by design.

---------------------------------------------------------------------------------

## 9. File index

- `PROTOCOL.md` - this document
- `probe_kvm.py` - probe + reference DVC decoder (Python 3, PIL only for PNG)
- `probe_frame.png` - first decoded frame
- `captures/probe1.log`, `captures/probe2.log` - probe output
- `captures/probe1.pcap`, `captures/probe2.pcap` - tcpdump of port 5900 (control is TLS)
- `captures/probe2_pkts/NNNNNN_TTTT.bin` - every raw video-socket packet (header+body)
- `viewer/src` - CFR output; `viewer/src_annot` - same with decoded strings
- `tools/DumpZ.java` + `tools/zstrings.txt` - all decoded obfuscated strings per class
- `tools/annotate.py` - builds `src_annot`
- `tools/DumpA.java` (arg map), `DumpPal.java` (PAL7), `DumpKeys.java` + `keymap_eng.txt`
  (Java key -> HID table), `DumpTP.java` (text-mode default colours)
