# iDRAC6 KVM relay - build notes & gotchas

Native C console relay for the Dell R210 II iDRAC6 ("the-bmc", <IDRAC_IP>).
Serves an HTML5 KVM console over HTTPS and bridges a browser WebSocket to the
iDRAC Avocent console on TCP/5900.

## Files
- `relay.c`      - the relay (mongoose 7.14 event loop + raw OpenSSL 1.0.2 per-client pthread).
- `embed.h`      - the console static files (index.html, app.js) baked in as C byte arrays.
                   Regenerate with the python snippet in "Regenerating embed.h" below if the
                   console UI changes. The wire format was NOT changed.
- `build.sh`     - cross-compile + OABI verification. Run inside the oabi-build:lenny container.
- `relay`        - the cross-compiled OABI ARM binary (statically linked).

## Build
```
podman run --rm \
  -v /opt/oabi-tc:/opt/oabi-tc \
  -v /opt/relay-build:/work \
  oabi-build:lenny /bin/bash /work/build.sh
```
Flags (validated): `-std=gnu99 -march=armv5te -Os -static -DMG_ENABLE_EPOLL=0
-DMG_TLS=MG_TLS_BUILTIN -I ssl-install/include` linking
`ssl-install/lib/libssl.a ssl-install/lib/libcrypto.a -lpthread -lrt -ldl`.
Soft-float is the default (no -mfloat-abi). epoll off -> poll. -lrt for
clock_gettime, -lpthread for the per-client KVM thread, -ldl for OpenSSL.

## OABI verification (must pass or it SIGILLs on the box)
The binary uses OABI syscalls: `swi 0x009000NN` (opcode `ef90xxxx`), never EABI `svc`.

GOTCHA: `grep -cE '\tswi'` returns 0 here because this grep does NOT expand `\t`
to a tab, so the CLAUDE.md check looked like it failed when it did not. Use a
literal tab, or just `grep -c swi` as CLAUDE.md literally says. Observed on this binary:
- `objdump -d relay | grep -c swi`        = 264  (> 0, PASS)
- `objdump -d relay | grep -c <TAB>svc`   = 0    (PASS, no EABI)
- real OABI opcodes `ef90[0-9a-f]{4} .swi.0x0090` = 158  (sanity: real syscalls present)

`build.sh` checks all three with a literal tab via `printf '\t'`.

## Architecture
- mongoose 7.14 event loop, HTTPS on :8443 with built-in TLS 1.3 + an **EC
  prime256v1** cert (RSA does NOT work with mongoose built-in TLS - confirmed).
- `/`            -> embedded index.html
- `/static/app.js` -> embedded app.js
- `/ws`          -> WebSocket upgrade; spawns one detached pthread (`kvm_thread`).
- The 5900 side is spoken with **raw OpenSSL 1.0.2** (SSLv23_client_method,
  SSL_VERIFY_NONE, cipher `ALL:@SECLEVEL=0`) because mongoose's built-in TLS
  cannot talk the iDRAC's legacy TLS. Control channel = TLS (caps 0x4), video =
  plain TCP (caps 0x1), exactly as the Go reference.
- Thread -> event loop: outgoing frames/events go on a mutex-guarded queue and
  the thread calls `mg_wakeup(mgr, conn_id, "", 0)`; the `MG_EV_WAKEUP` handler
  drains the queue with `mg_ws_send`. The large (~190 KB) video frames are NOT
  pushed through the wakeup socketpair - only a zero-length nudge is - so there
  is no truncation/interleave risk.
- Event loop -> thread: browser input (WS text JSON) is parsed with mongoose's
  `mg_json_get_*` and pushed to a mutex-guarded input queue the thread drains.
- Session lifetime: refcount 2 (ws side + thread); whoever finishes last frees.
  On `MG_EV_CLOSE` the ws side sets `stop=1`; the thread notices within 200 ms
  (its poll timeout) and exits, closing both 5900 sockets.

## KVM protocol (ported from kvm.go / PROTOCOL.md, unchanged wire format)
APCP hello (53 B) -> TLS wrap control -> LoginRequest 0x0102 -> LoginResponse
0x8305 -> video channel APCP + VideoChannelAuth -> on 0x84 send VideoEnable/
SetDisplayArea/SetScaleMode1to1/focus -> reassemble 0x81/82/83/8A BOF..EOF ->
emit WS binary `0x01 + be16 mode + be16 w + be16 h + data`. Keepalive 0x0400/
0x0420 every 2 s; video ack every 20 packets on the video socket. Input: key
0x0200, mouse 0x0201, focus 0x0208, refresh 0x0301, power 0x0424 (gated off).

## EC cert auto-generation (startup)
`ensure_cert()` generates `/flash/data0/etc/oob/relay_ec.{crt,key}` via the
box's `openssl` if missing. GOTCHAS on the box (openssl 1.0.2h):
- There is NO default openssl.cnf (points at a dead build path) -> `openssl req`
  fails with "Unable to load config info". Fixed by writing a minimal
  `/tmp/oob_ssl.cnf` (`[req] distinguished_name=dn; prompt=no; [dn] CN=idrac-oob`)
  and passing `-config`.
- `~/.rnd` is not writable -> set `RANDFILE=/tmp/.oob_rnd`. (The resulting
  "unable to write random state" / openssl.cnf warnings are harmless; cert is valid.)
- `ecparam -genkey` is used for the key (prime256v1 == secp256r1).

## On-box run (what the relay expects)
```
# iDRAC root creds are taken from the environment (video login is required to
# get any picture; it is a read-only view session).
IDRAC_USER=root IDRAC_PASS=<idrac-root-pw> /tmp/relay >/tmp/relay.log 2>&1 &
```
Then browse to https://<box>:8443/ . The page connects to /ws anonymously and
the relay logs into 5900 with those creds. Power control is gated OFF
(`allow_power=0` in `start_session`); flip it on only deliberately.

Backgrounding over ssh: busybox has **no `setsid` and no `nohup`**. Detach with
`(cmd </dev/null >/tmp/relay.log 2>&1 &)` - redirecting all three fds frees the
ssh channel so the session returns.

## Validation performed from the host (NO input/power sent)
- `curl -k https://<IDRAC_IP>:8443/`           -> 200, index.html (2098 B), TLS 1.3.
- `curl -k https://<IDRAC_IP>:8443/static/app.js` -> 200, app.js (10829 B exact).
- Raw WS-over-TLS client -> `101 Switching Protocols`, event
  `{"t":"status","msg":"connected"}` (= 5900 control login + video auth OK),
  then a real video frame: **mode=0x0081, 1024x768, datalen=191021** (full DVC
  frame reassembled BOF..EOF), 2 frames received. No keyboard/mouse/power sent.

## Coordinator notes (live-on-box)
- The binary is at `/tmp/relay` on the box; cert/key already generated at
  `/flash/data0/etc/oob/relay_ec.{crt,key}`. Delete them to re-test generation.
- glibc 2.3.6 here uses **LinuxThreads**, so each pthread shows as its own PID in
  `ps` (you will see a main `/tmp/relay` plus a LinuxThreads manager and one per
  active session). This is normal, not a fork bug.
- Keyboard/mouse/power input paths are implemented and compiled but were NOT
  exercised against the live host per instructions. The browser-side input, the
  JSON parsing, and the BEEF framing mirror kvm.go exactly; the first real
  keypress/mouse move is the thing left to confirm interactively.
- Host power control is intentionally disabled server-side; to allow it, set
  `s->allow_power = 1` in `start_session()` and rebuild.
- `/tmp/relay.log` came back empty (mongoose info logs are not emitted at the
  default level in this build); add `mg_log_set(MG_LL_DEBUG)` in main() if you
  want verbose logs while testing.

## Regenerating embed.h
```
python3 - <<'PY'
def emit(name,path):
    b=open(path,'rb').read(); o=["static const unsigned char %s[]={"%name]; ln=""
    for by in b:
        ln+="%d,"%by
        if len(ln)>110: o.append(ln); ln=""
    if ln:o.append(ln)
    o+=["};","static const unsigned int %s_len=%d;"%(name,len(b))]; return "\n".join(o)
base="/opt/network/discovery/idrac6/console/static/"
open("embed.h","w").write("/* generated */\n"+emit("EMB_INDEX",base+"index.html")+"\n"+emit("EMB_APPJS",base+"app.js")+"\n")
PY
```
