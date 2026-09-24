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
