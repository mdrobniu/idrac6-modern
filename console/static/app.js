'use strict';
// iDRAC6 HTML5 console - browser side.
// Decodes the Avocent DVC video stream natively (port of the Java decoder /
// probe_kvm.py DVC class) and sends keyboard/mouse over the WebSocket.

// ---- 7-bit palette (mode 0x82), from com.avocent.kvm.d.i.b()
const PAL7 = [
 0x000000,0x000046,0x00007F,0x0000BF,0x004600,0x004646,0x00467F,0x0046BF,
 0x007F00,0x007F46,0x007F7F,0x007FBF,0x00BF00,0x00BF46,0x00BF7F,0x00BFBF,
 0x460000,0x460046,0x46007F,0x4600BF,0x464600,0x464646,0x46467F,0x4646BF,
 0x467F00,0x467F46,0x467F7F,0x467FBF,0x46BF00,0x46BF46,0x46BF7F,0x46BFBF,
 0x7F0000,0x7F0046,0x7F007F,0x7F00BF,0x7F4600,0x7F4646,0x7F467F,0x7F46BF,
 0x7F7F00,0x7F7F46,0x7F7F7F,0x7F7FBF,0x7FBF00,0x7FBF46,0x7FBF7F,0x7FBFBF,
 0xBF0000,0xBF0046,0xBF007F,0xBF00BF,0xBF4600,0xBF4646,0xBF467F,0xBF46BF,
 0xBF7F00,0xBF7F46,0xBF7F7F,0xBF7FBF,0xBFBF00,0xBFBF46,0xBFBF7F,0xBFBFBF,
 0x0000FF,0x4600FF,0x7F00FF,0xBF00FF,0x0046FF,0x4646FF,0x7F46FF,0xBF46FF,
 0x007FFF,0x467FFF,0x7F7FFF,0xBF7FFF,0x00BFFF,0x46BFFF,0x7FBFFF,0xBFBFFF,
 0x00FF00,0x00FF46,0x00FF7F,0x00FFBF,0x46FF00,0x46FF46,0x46FF7F,0x46FFBF,
 0x7FFF00,0x7FFF46,0x7FFF7F,0x7FFFBF,0xBFFF00,0xBFFF46,0xBFFF7F,0xBFFFBF,
 0xFF0000,0xFF0046,0xFF007F,0xFF00BF,0xFF4600,0xFF4646,0xFF467F,0xFF46BF,
 0xFF7F00,0xFF7F46,0xFF7F7F,0xFF7FBF,0xFFBF00,0xFFBF46,0xFFBF7F,0xFFBFBF,
 0x00FFFF,0x46FFFF,0x7FFFFF,0xBFFFFF,0xFF00FF,0xFF46FF,0xFF7FFF,0xFFBFFF,
 0xFFFF00,0xFFFF46,0xFFFF7F,0xFFFFBF,0xFFFFFF,0x5F5F5F,0x9F9F9F,0xDFDFDF];

class DVC {
  constructor() { this.w = 0; this.h = 0; this.fb = new Int32Array(0); this.mode = 0; }
  resize(w, h) {
    if (w !== this.w || h !== this.h) {
      this.w = w; this.h = h; this.fb = new Int32Array(w * h);
    }
  }
  // 5-bit run length, LE, continuation bytes share top 3 bits of the opcode
  runlen(d, i, op, out) {
    const top = op & 0xE0; let n = op & 0x1F, k = 1;
    while (k < 5 && i < d.length && (d[i] & 0xE0) === top) { n |= (d[i] & 0x1F) << (5 * k); i++; k++; }
    const m = this.mode & 0xFF;
    if (m === 0x82 || m === 0x83) n += 2;
    out.i = i; return n;
  }
  pixel(d, i, op, out) {
    const m = this.mode & 0xFF; let v;
    if (m === 0x81) {                       // 15-bit RGB555
      v = ((op & 0x7F) << 8) | d[i]; i++;
      out.i = i;
      return (((v & 0x7C00) >> 7) << 16) | (((v & 0x3E0) >> 2) << 8) | ((v & 0x1F) << 3);
    }
    if (m === 0x8A) {                       // 23-bit 8-8-7
      v = ((op & 0x7F) << 16) | (d[i] << 8) | d[i + 1]; i += 2;
      out.i = i;
      return (((v & 0x7F8000) >> 15) << 16) | (((v & 0x7F80) >> 7) << 8) | ((v & 0x7F) << 1);
    }
    if (m === 0x83) {                       // 7-bit grey
      const g = (op & 0x7F) << 1; out.i = i; return (g << 16) | (g << 8) | g;
    }
    out.i = i; return PAL7[op & 0x7F];       // 7-bit palette
  }
  decode(d) {
    const fb = this.fb, W = this.w, N = fb.length;
    let i = 0, pos = 0; const L = d.length; const out = { i: 0 };
    while (i < L) {
      const op = d[i]; i++;
      const t = op & 0xE0;
      if (t === 0x00) {                      // no change
        const n = this.runlen(d, i, op, out); i = out.i; pos += n;
      } else if (t === 0x40) {               // copy above
        const n = this.runlen(d, i, op, out); i = out.i;
        const src = pos - W;
        if (src >= 0) {
          let c = Math.min(n, N - pos, N - src);
          for (let k = 0; k < c; k++) fb[pos + k] = fb[src + k];
          if (c > 0) pos += c;
        }
      } else if (t === 0x20) {               // copy left (repeat last)
        const n = this.runlen(d, i, op, out); i = out.i;
        const c = pos >= 1 ? fb[pos - 1] : 0;
        let cnt = Math.min(n, N - pos);
        for (let k = 0; k < cnt; k++) fb[pos + k] = c;
        if (cnt > 0) pos += cnt;
      } else if (t === 0x60) {               // 2-colour bitmap
        const c0 = (pos > 0 && pos <= N) ? fb[pos - 1] : 0; let c1 = c0;
        for (let k = pos - 1; k >= 0; k--) { if (fb[k] !== c0) { c1 = fb[k]; break; } }
        let bits = [(op >> 3) & 1, (op >> 2) & 1, (op >> 1) & 1, op & 1];
        let cont = op & 0x10;
        for (;;) {
          for (const b of bits) { if (pos < N) fb[pos] = b ? c1 : c0; pos++; }
          if (!cont || i >= L) break;
          const nb = d[i]; i++;
          bits = [(nb >> 6) & 1, (nb >> 5) & 1, (nb >> 4) & 1, (nb >> 3) & 1,
                  (nb >> 2) & 1, (nb >> 1) & 1, nb & 1];
          cont = nb & 0x80;
        }
      } else {                               // make pixel
        const c = this.pixel(d, i, op, out); i = out.i;
        if (pos < N) fb[pos] = c; pos++;
      }
    }
  }
  blit(imgData) {
    const fb = this.fb, px = imgData.data;
    for (let j = 0, p = 0; j < fb.length; j++, p += 4) {
      const v = fb[j];
      px[p] = (v >> 16) & 255; px[p + 1] = (v >> 8) & 255; px[p + 2] = v & 255; px[p + 3] = 255;
    }
  }
}

// ---- KeyboardEvent.code -> USB HID usage (page 7)
const HID = {
  KeyA:0x04,KeyB:0x05,KeyC:0x06,KeyD:0x07,KeyE:0x08,KeyF:0x09,KeyG:0x0A,KeyH:0x0B,KeyI:0x0C,
  KeyJ:0x0D,KeyK:0x0E,KeyL:0x0F,KeyM:0x10,KeyN:0x11,KeyO:0x12,KeyP:0x13,KeyQ:0x14,KeyR:0x15,
  KeyS:0x16,KeyT:0x17,KeyU:0x18,KeyV:0x19,KeyW:0x1A,KeyX:0x1B,KeyY:0x1C,KeyZ:0x1D,
  Digit1:0x1E,Digit2:0x1F,Digit3:0x20,Digit4:0x21,Digit5:0x22,Digit6:0x23,Digit7:0x24,
  Digit8:0x25,Digit9:0x26,Digit0:0x27,Enter:0x28,Escape:0x29,Backspace:0x2A,Tab:0x2B,
  Space:0x2C,Minus:0x2D,Equal:0x2E,BracketLeft:0x2F,BracketRight:0x30,Backslash:0x31,
  Semicolon:0x33,Quote:0x34,Backquote:0x35,Comma:0x36,Period:0x37,Slash:0x38,CapsLock:0x39,
  F1:0x3A,F2:0x3B,F3:0x3C,F4:0x3D,F5:0x3E,F6:0x3F,F7:0x40,F8:0x41,F9:0x42,F10:0x43,F11:0x44,
  F12:0x45,PrintScreen:0x46,ScrollLock:0x47,Pause:0x48,Insert:0x49,Home:0x4A,PageUp:0x4B,
  Delete:0x4C,End:0x4D,PageDown:0x4E,ArrowRight:0x4F,ArrowLeft:0x50,ArrowDown:0x51,ArrowUp:0x52,
  NumLock:0x53,NumpadDivide:0x54,NumpadMultiply:0x55,NumpadSubtract:0x56,NumpadAdd:0x57,
  NumpadEnter:0x58,Numpad1:0x59,Numpad2:0x5A,Numpad3:0x5B,Numpad4:0x5C,Numpad5:0x5D,
  Numpad6:0x5E,Numpad7:0x5F,Numpad8:0x60,Numpad9:0x61,Numpad0:0x62,NumpadDecimal:0x63,
  ContextMenu:0x65,ControlLeft:0xE0,ShiftLeft:0xE1,AltLeft:0xE2,MetaLeft:0xE3,
  ControlRight:0xE4,ShiftRight:0xE5,AltRight:0xE6,MetaRight:0xE7
};

const dvc = new DVC();
let canvas, ctx, img, ws, buttons = 0, connected = false;
let lastX = 0, lastY = 0, grabbed = false;

function setStatus(s, cls) {
  const el = document.getElementById('status');
  el.textContent = s; el.className = cls || '';
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(proto + '://' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => setStatus('handshaking...', 'warn');
  ws.onclose = () => { setStatus('disconnected - retrying in 3s', 'err'); connected = false;
                       setTimeout(connect, 3000); };
  ws.onerror = () => {};
  ws.onmessage = (ev) => {
    const b = new Uint8Array(ev.data);
    if (b[0] === 0x01) {                      // video frame
      const dv = new DataView(ev.data);
      const mode = dv.getUint16(1), w = dv.getUint16(3), h = dv.getUint16(5);
      const data = b.subarray(7);
      dvc.mode = mode;
      if (w !== dvc.w || h !== dvc.h) {
        dvc.resize(w, h);
        canvas.width = w; canvas.height = h;
        img = ctx.createImageData(w, h);
        document.getElementById('res').textContent = w + 'x' + h;
      }
      dvc.decode(data);
      dvc.blit(img);
      ctx.putImageData(img, 0, 0);
      if (!connected) { connected = true; setStatus('connected', 'ok'); }
    } else if (b[0] === 0x02) {               // JSON event
      const ev2 = JSON.parse(new TextDecoder().decode(b.subarray(1)));
      handleEvent(ev2);
    }
  };
}

function handleEvent(e) {
  if (e.t === 'error') setStatus('error: ' + e.msg, 'err');
  else if (e.t === 'status') setStatus(e.msg, 'warn');
  else if (e.t === 'closed') setStatus('session closed', 'err');
  else if (e.t === 'power_status') {
    document.getElementById('power').textContent = 'host power: ' + (e.on ? 'ON' : 'off');
  } else if (e.t === 'info') setStatus(e.msg, 'warn');
}

function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }

function fbCoords(ev) {
  const r = canvas.getBoundingClientRect();
  const x = Math.round((ev.clientX - r.left) * (canvas.width / r.width));
  const y = Math.round((ev.clientY - r.top) * (canvas.height / r.height));
  lastX = Math.max(0, Math.min(canvas.width - 1, x));
  lastY = Math.max(0, Math.min(canvas.height - 1, y));
  return [lastX, lastY];
}

function initInput() {
  canvas.addEventListener('mousemove', (e) => { const [x, y] = fbCoords(e); send({ t: 'mouse', x, y, buttons, wheel: 0 }); });
  canvas.addEventListener('mousedown', (e) => {
    e.preventDefault(); canvas.focus();
    const bit = e.button === 0 ? 1 : e.button === 2 ? 2 : 4; buttons |= bit;
    const [x, y] = fbCoords(e); send({ t: 'mouse', x, y, buttons, wheel: 0 });
  });
  window.addEventListener('mouseup', (e) => {
    const bit = e.button === 0 ? 1 : e.button === 2 ? 2 : 4; buttons &= ~bit;
    send({ t: 'mouse', x: lastX, y: lastY, buttons, wheel: 0 });
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    send({ t: 'mouse', x: lastX, y: lastY, buttons, wheel: e.deltaY > 0 ? -1 : 1 });
  }, { passive: false });

  const keyHandler = (down) => (e) => {
    if (!grabbed) return;
    const code = HID[e.code];
    if (code === undefined) return;
    e.preventDefault();
    send({ t: 'key', code, down });
  };
  window.addEventListener('keydown', keyHandler(true));
  window.addEventListener('keyup', keyHandler(false));
}

function cad() {   // Ctrl-Alt-Del
  for (const c of [0xE0, 0xE2, 0x4C]) send({ t: 'key', code: c, down: true });
  setTimeout(() => { for (const c of [0x4C, 0xE2, 0xE0]) send({ t: 'key', code: c, down: false }); }, 120);
}

window.addEventListener('DOMContentLoaded', () => {
  canvas = document.getElementById('screen');
  canvas.tabIndex = 0;
  ctx = canvas.getContext('2d', { alpha: false });
  initInput();
  document.getElementById('grab').onclick = () => {
    grabbed = !grabbed;
    document.getElementById('grab').textContent = grabbed ? 'Release keyboard' : 'Grab keyboard';
    document.getElementById('grab').className = grabbed ? 'on' : '';
    send({ t: 'focus', on: grabbed });
    if (grabbed) canvas.focus();
  };
  document.getElementById('refresh').onclick = () => send({ t: 'refresh' });
  document.getElementById('cad').onclick = cad;
  const pc = document.getElementById('powerctl');
  if (pc) pc.onchange = () => {
    const v = parseInt(pc.value, 10);
    pc.value = '';
    if (v && confirm('Send power command ' + v + ' to the host?')) send({ t: 'power', state: v });
  };
  connect();
});
