//
// A game controller, passed through to the engine.
//
// The engine reads controllers itself now (WinQuake/in_pad.c): its buttons
// are keys bound in the game's own Options -> Controls -> Customize Controls,
// its sticks move and look, and its settings are on the Controls page -- the
// same in this container and in the Linux desktop build. What the engine
// cannot do from inside a container is see the pad, which is plugged into the
// machine running this browser. So this file reads it through the Gamepad API
// and sends its state across, and that is all it does: no bindings, no stick
// maths, nothing to configure here.
//
// It goes over its own WebSocket to the same port as the picture and the
// sound (token=pad), which websockify hands to a TCP port the engine listens
// on. Two messages, little-endian, as in_pad.c reads them:
//
//   'P' connected buttons:u32 lx ly rx ry:i16 lt rt:u8     16 bytes
//   'N' length name                                         the pad's name
//
// Buttons are in the standard layout's order: A B X Y LB RB LT RT View Menu,
// the stick clicks, the d-pad, Guide.
//
// And one message back, when the game wants the pad to vibrate (Options ->
// Controls -> Vibration):
//
//   'R' low high:u16 milliseconds:u16                        7 bytes
//
// played through the browser's vibrationActuator where it has one, and
// hapticActuators (Firefox's) where it has that instead.
//

// "Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)" is
// what the browser calls it; the useful half is in front of the bracket.
export function padName(id) {
  const s = String(id || '');
  const cut = s.indexOf(' (');
  return (cut > 0 ? s.slice(0, cut) : s).trim() || 'Controller';
}

const HEARTBEAT_MS = 250;   // resent this often even when nothing changes
const RETRY_MS = 2000;      // and the connection tried again this often

export class QuakePadBridge {
  constructor({ url, onChange } = {}) {
    this._url = url;
    this._onChange = onChange || (() => {});
    this._sock = null;
    this._open = false;
    this._retryAt = 0;
    this._last = '';
    this._lastAt = 0;
    this._name = '';
    this._sentName = '';
    this._pad = null;
    this._rx = new Uint8Array(0);   // bytes from the engine not yet used
  }

  // The first pad the browser reports, preferring one it knows the layout of.
  pad() {
    const all = Array.from((navigator.getGamepads && navigator.getGamepads()) || [])
      .filter(p => p && p.connected);
    return all.find(p => p.mapping === 'standard') || all[0] || null;
  }

  available() {
    return !!this._pad;
  }

  connected() {
    return this._open;
  }

  // Once a frame. `active` is whether the game is being played: with the
  // start screen up, or the tab behind another, the pad is reported with
  // nothing held, so nothing stays pressed in a game nobody is looking at.
  poll(active) {
    const pad = this.pad();
    const had = this._pad;
    this._pad = pad;
    if (!!had !== !!pad || (pad && had && pad.id !== had.id)) this._onChange();

    this._connect();
    if (!this._open) return;

    const name = pad ? padName(pad.id) : '';
    if (name && name !== this._sentName) {
      const bytes = new TextEncoder().encode(name).slice(0, 60);
      const m = new Uint8Array(2 + bytes.length);
      m[0] = 'N'.charCodeAt(0);
      m[1] = bytes.length;
      m.set(bytes, 2);
      this._sock.send(m);
      this._sentName = name;
    }

    const msg = this._state(pad, active);
    const key = msg.join(',');
    const now = performance.now();
    if (key === this._last && now - this._lastAt < HEARTBEAT_MS) return;
    this._last = key;
    this._lastAt = now;
    this._sock.send(msg);
  }

  _state(pad, active) {
    const m = new Uint8Array(16);
    const v = new DataView(m.buffer);
    m[0] = 'P'.charCodeAt(0);
    m[1] = pad ? 1 : 0;
    if (!pad || !active) return m;

    let bits = 0;
    pad.buttons.forEach((b, i) => {
      if (i < 32 && b && (b.pressed || b.value > 0.5)) bits |= (1 << i);
    });
    // The triggers go as their own analog values as well; in_pad.c decides
    // where a squeeze becomes a press.
    bits &= ~((1 << 6) | (1 << 7));
    v.setUint32(2, bits >>> 0, true);

    const ax = i => {
      const a = pad.axes[i];
      return typeof a === 'number' ? Math.max(-1, Math.min(1, a)) : 0;
    };
    v.setInt16(6, Math.round(ax(0) * 32767), true);
    v.setInt16(8, Math.round(ax(1) * 32767), true);
    v.setInt16(10, Math.round(ax(2) * 32767), true);
    v.setInt16(12, Math.round(ax(3) * 32767), true);

    const trig = i => {
      const b = pad.buttons[i];
      if (!b) return 0;
      return Math.round(Math.max(b.value || 0, b.pressed ? 1 : 0) * 255);
    };
    m[14] = trig(6);
    m[15] = trig(7);
    return m;
  }

  // websockify passes the engine's bytes on however TCP happened to cut them,
  // so a message can arrive in pieces, or several in one.
  _receive(data) {
    if (!(data instanceof ArrayBuffer)) return;
    const add = new Uint8Array(data);
    const buf = new Uint8Array(this._rx.length + add.length);
    buf.set(this._rx);
    buf.set(add, this._rx.length);
    let at = 0;
    while (buf.length - at >= 7) {
      if (buf[at] !== 82) {           // not 'R': lost the thread, drop it all
        at = buf.length;
        break;
      }
      const v = new DataView(buf.buffer, at);
      this._rumble(v.getUint16(1, true) / 65535, v.getUint16(3, true) / 65535,
                   v.getUint16(5, true));
      at += 7;
    }
    this._rx = buf.slice(at);
  }

  _rumble(strong, weak, ms) {
    const pad = this._pad;
    if (!pad) return;
    try {
      const va = pad.vibrationActuator;
      if (va && va.playEffect) {
        va.playEffect(va.type || 'dual-rumble', {
          startDelay: 0, duration: ms,
          strongMagnitude: strong, weakMagnitude: weak,
        }).catch(() => {});
        return;
      }
      const ha = pad.hapticActuators && pad.hapticActuators[0];
      if (ha && ha.pulse) ha.pulse(Math.max(strong, weak), ms).catch(() => {});
    } catch (e) {
      // a pad that says it can and then cannot: nothing to be done about it
    }
  }

  _connect() {
    if (this._sock || performance.now() < this._retryAt) return;
    this._retryAt = performance.now() + RETRY_MS;

    let s;
    try {
      s = new WebSocket(this._url, 'binary');
    } catch (e) {
      return;
    }
    s.binaryType = 'arraybuffer';
    s.onmessage = (ev) => this._receive(ev.data);
    s.onopen = () => {
      this._open = true;
      this._sentName = '';
      this._last = '';
      this._onChange();
    };
    // The engine restarts when a game is picked from Options -> Game / Mod,
    // and its end of this goes with it; the next poll connects again.
    s.onclose = s.onerror = () => {
      if (this._sock !== s) return;
      this._sock = null;
      if (this._open) {
        this._open = false;
        this._onChange();
      }
    };
    this._sock = s;
  }
}
