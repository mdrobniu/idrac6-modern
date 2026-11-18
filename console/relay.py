#!/usr/bin/env python3
"""
iDRAC6 HTML5 console relay.

A small web server that bridges a browser (WebSocket) to a Dell iDRAC6
Avocent virtual console (TCP/5900). The browser does the video decoding
(DVC codec ported to JS in static/app.js); this backend only:
  - logs in to the iDRAC web UI to fetch the one-time KVM credentials,
  - opens the control (TLS) + video (plain) channels and authenticates,
  - reassembles each DVC video frame (BOF..EOF) and ships it to the browser,
  - keepalives (2 s) and video-acks (every 20 pkts) on its own,
  - turns browser key/mouse/refresh[/power] events into console messages.

Protocol: see ../PROTOCOL.md. Reference decoder: ../probe_kvm.py.
No host is touched unless power control is explicitly enabled (--allow-power).

Env / args:
  IDRAC_HOST (<IDRAC_IP>)  IDRAC_USER (root)  IDRAC_PASS  LISTEN (0.0.0.0)
  PORT (8099)  ALLOW_POWER (0)
"""
import argparse, asyncio, os, re, socket, ssl, struct, threading, time, urllib.parse
import http.client
from aiohttp import web, WSMsgType

HDR = struct.Struct('>4sHH')          # BEEF framing: magic, type, total length
DEBUG = os.environ.get('DEBUG', '0') == '1'


def now():
    return time.strftime('%H:%M:%S')


def logline(*a):
    print(now(), *a, flush=True)


# ----------------------------------------------------------------- web login
def _https(host):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.set_ciphers('DEFAULT:@SECLEVEL=0')
    return http.client.HTTPSConnection(host, 443, context=ctx, timeout=20)


def web_login(host, user, pw):
    """Returns (session_cookie, st2, viewer_args{ip,kmport,vport,user,passwd,...})."""
    c = _https(host)
    c.request('GET', '/login.html')
    r = c.getresponse(); r.read()
    m = re.search(r'_appwebSessionId_=([0-9a-f]+)', r.getheader('Set-Cookie') or '')
    if not m:
        raise RuntimeError('no appweb session cookie')
    cookie = m.group(1)
    body = 'user=%s&password=%s' % (urllib.parse.quote(user), urllib.parse.quote(pw))
    c.request('POST', '/data/login', body,
              {'Cookie': '_appwebSessionId_=' + cookie,
               'Content-Type': 'application/x-www-form-urlencoded'})
    r = c.getresponse(); x = r.read().decode()
    mm = re.search(r'ST1=([0-9a-f]+),ST2=([0-9a-f]+)', x)
    if '<authResult>0</authResult>' not in x or not mm:
        raise RuntimeError('web login failed (bad credentials?)')
    st1, st2 = mm.groups()
    title = urllib.parse.quote('html5, User:' + user)
    path = '/viewer.jnlp(%s@0@%s@%d@ST1=%s)' % (host, title, int(time.time() * 1000), st1)
    c.request('GET', path, headers={'Cookie': '_appwebSessionId_=' + cookie})
    r = c.getresponse(); jnlp = r.read().decode()
    args = dict(re.findall(r'<argument>([^=<]+)=([^<]*)</argument>', jnlp))
    if 'user' not in args or 'passwd' not in args:
        raise RuntimeError('viewer.jnlp had no KVM credentials')
    return cookie, st2, args


def web_logout(host, cookie, st2):
    try:
        c = _https(host)
        c.request('GET', '/data/logout',
                  headers={'Cookie': '_appwebSessionId_=' + cookie, 'ST2': st2})
        c.getresponse().read()
    except Exception as e:
        logline('logout error', e)


# ------------------------------------------------------------ socket helpers
def recvn(s, n):
    b = bytearray()
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise EOFError('socket closed')
        b += d
    return bytes(b)


def apcp(s, chan_type, caps=5):
    rnd = os.urandom(32)
    pkt = b'APCP' + struct.pack('>IHHBBBBIB', 53, 0x0100, 0, chan_type, 2, 34, 0, caps, 32) + rnd
    s.sendall(pkt)
    hdr = recvn(s, 8)
    _, ln = struct.unpack('>4sI', hdr)
    body = recvn(s, ln - 8)
    typ, z, maj, mnr, cap, port, rl = struct.unpack('>HHBBIHB', body[:13])
    return dict(caps=cap, port=port)


def tls_wrap(sock, host):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.set_ciphers('ALL:@SECLEVEL=0')
    return ctx.wrap_socket(sock, server_hostname=host)


# --------------------------------------------------------------- KVM session
class KvmSession:
    """Runs the iDRAC console in its own threads; pushes frames via on_frame()."""

    def __init__(self, host, user, pw, allow_power, on_frame, on_event):
        self.host, self.user, self.pw = host, user, pw
        self.allow_power = allow_power
        self.on_frame = on_frame      # (mode:int, w:int, h:int, data:bytes)
        self.on_event = on_event      # (dict) -> pushed to browser as JSON
        self.cs = self.vs = None
        self.clock = threading.Lock()
        self.alive = True
        self.cookie = self.st2 = None
        self.server_id = b'current1'
        self.disp = (0, 0)

    # ---- control-socket senders (thread-safe)
    def _beef(self, typ, body=b''):
        body = body + b'\0' * max(0, 8 - len(body))
        with self.clock:
            if self.cs:
                self.cs.sendall(HDR.pack(b'BEEF', typ, 8 + len(body)) + body)

    def key(self, code, down):
        # 0x0200: [0]=0, [1]=0 press / 1 release, [2..3]=u16 HID usage
        if DEBUG:
            logline('KEY code=0x%02x down=%s' % (code, down))
        self._beef(0x0200, bytes([0, 0 if down else 1]) + struct.pack('>H', code & 0xFFFF))

    def mouse(self, x, y, buttons, wheel):
        # 0x0201 absolute: [0]=0,[1]=buttons,[2..3]=x,[4..5]=y,[6..7]=wheel s16
        self._beef(0x0201, bytes([0, buttons & 7]) +
                   struct.pack('>HHh', max(0, x), max(0, y), wheel))

    def refresh(self):
        self._beef(0x0301)

    def focus(self, gained):
        # 0x0208 FocusControl: the host gates KEYBOARD input on this (mouse does not need it).
        if DEBUG:
            logline('FOCUS gained=%s' % gained)
        self._beef(0x0208, bytes([1 if gained else 0]))

    def power(self, state):
        if not self.allow_power:
            self.on_event({'t': 'error', 'msg': 'power control disabled on this host'})
            return
        if state not in (1, 2, 3, 4, 5):
            return
        sid = self.server_id
        body = struct.pack('>H', len(sid)) + sid + bytes([state])
        with self.clock:
            if self.cs:
                self.cs.sendall(HDR.pack(b'BEEF', 0x0424, 8 + len(body)) + body)
        self.on_event({'t': 'info', 'msg': 'power command %d sent' % state})

    def close(self):
        self.alive = False
        for s in (self.cs, self.vs):
            try:
                s and s.close()
            except Exception:
                pass
        if self.cookie:
            web_logout(self.host, self.cookie, self.st2)

    # ---- main run (blocking; call in a thread)
    def run(self):
        try:
            self.cookie, self.st2, args = web_login(self.host, self.user, self.pw)
            host = args.get('ip', self.host)
            kmport, vport = int(args.get('kmport', 5900)), int(args.get('vport', 5900))
            kuser, kpass = args['user'], args['passwd']

            cs = socket.create_connection((host, kmport), timeout=30)
            cs.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            a = apcp(cs, 3)
            if a['port']:
                cs.close(); cs = socket.create_connection((host, a['port']), timeout=30)
            if not a['caps'] & 1 and a['caps'] & 4:
                cs = tls_wrap(cs, host)
            self.cs = cs

            crand = int.from_bytes(os.urandom(3), 'big') % 10000000
            lb = (bytes([len(kuser)]) + kuser.encode().ljust(96, b'\0') +
                  bytes([len(kpass)]) + kpass.encode().ljust(96, b'\0') +
                  b'\0' * 8 + bytes([1, 0]) + struct.pack('>I', crand) + b'\0')
            cs.sendall(HDR.pack(b'BEEF', 0x0102, 8 + len(lb)) + lb)
            h = recvn(cs, 8); _, typ, ln = HDR.unpack(h); b = recvn(cs, ln - 8)
            if typ != 0x8305 or b[0] != 0:
                raise RuntimeError('KVM login refused (result %d)' % (b[0] if b else -1))
            vkey = struct.unpack('>I', b[4:8])[0]

            vs = socket.create_connection((host, vport), timeout=60)
            vs.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            va = apcp(vs, 4)
            if va['port']:
                vs.close(); vs = socket.create_connection((host, va['port']), timeout=60)
            if not va['caps'] & 1 and va['caps'] & 4:
                vs = tls_wrap(vs, host)
            self.vs = vs
            vs.sendall(bytes([0, 0, 0, 0, 1, 1, 0, 16]) + struct.pack('>II', crand, vkey))

            self.on_event({'t': 'status', 'msg': 'connected'})
            threading.Thread(target=self._ctrl_reader, daemon=True).start()
            threading.Thread(target=self._keepalive, daemon=True).start()
            self._video_loop(vs)
        except Exception as e:
            logline('session error:', repr(e))
            self.on_event({'t': 'error', 'msg': str(e)})
        finally:
            self.close()
            self.on_event({'t': 'closed'})

    def _ctrl_reader(self):
        try:
            while self.alive:
                h = recvn(self.cs, 8); _, typ, ln = HDR.unpack(h); b = recvn(self.cs, ln - 8)
                if typ == 0x8430:                       # power/servers status
                    m = re.search(rb'(current\d+)', b)
                    if m:
                        self.server_id = m.group(1)
                    st = b[-1] & 3 if b else 0
                    self.on_event({'t': 'power_status', 'on': bool(st & 1)})
                elif typ == 0x8100:                     # keyboard LED reply
                    if DEBUG:
                        logline('LED reply 0x8100 = %s' % (b[0] if b else None))
                elif typ == 0x8301:                     # disconnect pending
                    self.on_event({'t': 'info', 'msg': 'server closing session'})
                    self.alive = False
        except Exception:
            self.alive = False

    def _keepalive(self):
        running = 0
        while self.alive:
            self._beef(0x0400)
            if self.disp != (0, 0):
                self._beef(0x0420)
            time.sleep(2)

    def _start_video(self):
        self._beef(0x030E, bytes([1, 1]))
        self._beef(0x0302, struct.pack('>HH', 1024, 768))
        self._beef(0x0304)
        self._beef(0x0208, bytes([1]))     # focus gained -> enable keyboard input
        if DEBUG:
            logline('start_video: VideoEnable/SetDisplayArea/Focus sent')
        self.disp = (1024, 768)

    def _video_loop(self, vs):
        vs.settimeout(5)
        buf = bytearray()
        started = False
        last_type = 0
        vcount = 0
        while self.alive:
            try:
                h = recvn(vs, 8); _, typ, ln = HDR.unpack(h); b = recvn(vs, ln - 8)
            except socket.timeout:
                continue
            except Exception:
                break
            low = typ & 0xFF
            if low == 0x84:                              # video connect -> start
                self._start_video()
            elif low in (0x81, 0x82, 0x83, 0x8A):
                if typ != last_type:
                    last_type = typ
                    self._beef(0x0403, bytes([1, 0, 0, 0]))
                height, width = struct.unpack('>HH', b[4:8])
                flags = b[8]
                data = b[12:]
                if flags & 1:                            # begin of frame
                    started = True
                    buf = bytearray()
                if started:
                    buf += data
                    if flags & 2:                        # end of frame
                        if self.disp != (width, height):
                            self.disp = (width, height)
                            self._beef(0x0302, struct.pack('>HH', width, height))
                        self.on_frame(typ, width, height, bytes(buf))
                        buf = bytearray()
                vcount += 1
            else:
                vcount += 1
            if vcount >= 20:
                try:
                    vs.sendall(HDR.pack(b'BEEF', 0, 16) + bytes([vcount & 0xFF]) + b'\0' * 7)
                except Exception:
                    break
                vcount = 0


# --------------------------------------------------------------- web server
async def ws_handler(request):
    app = request.app
    ws = web.WebSocketResponse(max_msg_size=4 * 1024 * 1024)
    await ws.prepare(request)
    loop = asyncio.get_event_loop()
    q = asyncio.Queue(maxsize=8)

    def on_frame(mode, w, h, data):
        # binary: 0x01 | mode u16 | w u16 | h u16 | data
        msg = b'\x01' + struct.pack('>HHH', mode, w, h) + data
        try:
            loop.call_soon_threadsafe(q.put_nowait, msg)
        except asyncio.QueueFull:
            pass
        except RuntimeError:
            pass

    def on_event(ev):
        import json
        try:
            loop.call_soon_threadsafe(q.put_nowait, b'\x02' + json.dumps(ev).encode())
        except Exception:
            pass

    sess = KvmSession(app['host'], app['user'], app['pass'], app['allow_power'],
                      on_frame, on_event)
    threading.Thread(target=sess.run, daemon=True).start()
    logline('client connected from', request.remote)

    async def pump():
        while True:
            msg = await q.get()
            if msg is None:
                break
            try:
                await ws.send_bytes(msg)
            except Exception:
                break
    pump_task = asyncio.ensure_future(pump())
    try:
        async for m in ws:
            if m.type != WSMsgType.TEXT:
                continue
            import json
            try:
                d = json.loads(m.data)
            except Exception:
                continue
            t = d.get('t')
            if t == 'key':
                await loop.run_in_executor(None, sess.key, int(d['code']), bool(d['down']))
            elif t == 'mouse':
                await loop.run_in_executor(None, sess.mouse, int(d['x']), int(d['y']),
                                           int(d.get('buttons', 0)), int(d.get('wheel', 0)))
            elif t == 'refresh':
                await loop.run_in_executor(None, sess.refresh)
            elif t == 'focus':
                await loop.run_in_executor(None, sess.focus, bool(d.get('on', True)))
            elif t == 'power':
                await loop.run_in_executor(None, sess.power, int(d.get('state', 0)))
    finally:
        sess.close()
        pump_task.cancel()
        logline('client disconnected')
    return ws


async def index(request):
    return web.FileResponse(os.path.join(os.path.dirname(__file__), 'static', 'index.html'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default=os.environ.get('IDRAC_HOST','127.0.0.1'))
    ap.add_argument('--user', default=os.environ.get('IDRAC_USER', 'root'))
    ap.add_argument('--password', default=os.environ.get('IDRAC_PASS', ''))
    ap.add_argument('--listen', default=os.environ.get('LISTEN', '0.0.0.0'))
    ap.add_argument('--port', type=int, default=int(os.environ.get('PORT', '8099')))
    ap.add_argument('--allow-power', action='store_true',
                    default=os.environ.get('ALLOW_POWER', '0') == '1')
    o = ap.parse_args()

    app = web.Application()
    app['host'], app['user'], app['pass'] = o.host, o.user, o.password
    app['allow_power'] = o.allow_power
    app.router.add_get('/', index)
    app.router.add_get('/ws', ws_handler)
    app.router.add_static('/static/', os.path.join(os.path.dirname(__file__), 'static'))
    logline('iDRAC console relay for %s on %s:%d (power=%s)' %
            (o.host, o.listen, o.port, 'ON' if o.allow_power else 'off'))
    web.run_app(app, host=o.listen, port=o.port, print=None)


if __name__ == '__main__':
    main()
