//
// The buffer between the container's clock and this machine's.
//
// The container sends at a steady real-time rate and the sound card consumes
// at its own, which are never quite the same. This absorbs the difference --
// padding when it runs dry, dropping the oldest frame when it runs long --
// and resamples to whatever rate the AudioContext turned out to be. Handing
// the browser a queue of scheduled buffers instead would drift until it
// either stuttered or fell behind.
//
// How much it holds is the point. Everything it holds is delay between
// pulling a trigger and hearing it, so it starts with as little as it can and
// asks for more only when that turns out not to be enough -- which depends on
// the machine, the browser and what else the page is doing, none of which can
// be known from here. A fixed figure is either too much for everyone or too
// little for somebody.
//
// Assigned onto globalThis rather than exported, because this file is loaded
// two ways: as an ordinary script by the page, and as a worklet module into
// the AudioWorkletGlobalScope, which does not share the page's. A module
// export would only reach one of them.
//
globalThis.QuakeRing = class QuakeRing {
  constructor(srcRate, outRate, targetMs, maxTargetMs, maxMs) {
    this.srcRate = srcRate;
    this.ratio   = srcRate / outRate;       // source frames per output frame
    this.max     = Math.ceil(maxMs * srcRate / 1000);

    this.minTarget = Math.ceil(targetMs * srcRate / 1000);
    this.maxTarget = Math.ceil(maxTargetMs * srcRate / 1000);
    this.target    = this.minTarget;

    // What one underrun costs, and how long it has to behave before the
    // cushion is given back: grow fast, shrink slowly, or a single hiccup
    // starts an oscillation.
    this.grow  = Math.ceil(0.02 * srcRate);   // 20 ms
    this.shrink = Math.ceil(0.01 * srcRate);  // 10 ms
    this.cleanFor = 0;
    this.cleanNeeded = 8 * srcRate;           // 8 seconds

    // Growth and decay have to be unequal or the target never settles: at
    // 20 ms up per underrun against 10 ms down every four seconds, a machine
    // that underruns every ten seconds sits exactly still, underrunning for
    // ever. Decaying half as fast lets it climb until they stop, then drift
    // back down to whatever this machine actually needs.

    // The two clocks do not agree, and nothing makes them. The container sends
    // on its own schedule, this machine plays on its sound card's, and every
    // block the browser is too busy to fetch leaves more sound sitting here
    // than was asked for -- permanently, since the only other thing that ever
    // removes any is the hard ceiling far above. Measured over half a minute,
    // that walked 35 ms of delay up to 195.
    //
    // So playback is nudged instead: run a fraction fast while there is too
    // much in hand, a fraction slow while there is too little. A couple of
    // per cent is inaudible on gunfire and door mechanisms -- far less
    // audible than dropping the frames outright, which clicks -- and it
    // corrects about 20 ms a second, which is faster than the drift arrives.
    this.maxDrift = 0.02;

    // How hard to push for a given error. The target itself sets the scale:
    // being 40 ms out matters more when 40 ms is all there is.
    this.driftGain = 8;

    this.left   = new Float32Array(this.max * 2);
    this.right  = new Float32Array(this.max * 2);
    this.size   = this.left.length;
    this.head   = 0;     // next write position
    this.tail   = 0;     // read position, fractional
    this.count  = 0;     // frames available
    this.started = false;
    this.underruns = 0;
  }

  // Interleaved stereo floats, as they came off the socket.
  push(frames) {
    const n = frames.length >> 1;

    for (let i = 0; i < n; i++) {
      if (this.count >= this.max) {
        // Running long: drop the oldest frame rather than refuse the newest,
        // so the delay shrinks instead of the sound tearing at the end.
        this.tail = (this.tail + 1) % this.size;
        this.count--;
      }

      this.left[this.head]  = frames[i*2];
      this.right[this.head] = frames[i*2 + 1];
      this.head = (this.head + 1) % this.size;
      this.count++;
    }
  }

  starve() {
    this.underruns++;
    this.started = false;
    this.cleanFor = 0;
    this.target = Math.min(this.maxTarget, this.target + this.grow);
  }

  read(l, r) {
    const n = l.length;

    // Wait for a cushion before starting, or the first note underruns
    // immediately and clicks.
    if (!this.started) {
      if (this.count < this.target) {
        l.fill(0);
        if (r !== l) r.fill(0);
        return;
      }
      this.started = true;
    }

    // How far from the target, as a fraction of it, turned into a playback
    // rate a little either side of true.
    let drift = (this.count - this.target) / (this.target * this.driftGain);
    if (drift >  this.maxDrift) drift =  this.maxDrift;
    if (drift < -this.maxDrift) drift = -this.maxDrift;
    const step = this.ratio * (1 + drift);
    this.drift = drift;

    for (let i = 0; i < n; i++) {
      if (this.count < 2) {
        // Dry. Silence is the honest answer; repeating the last frame buzzes.
        l[i] = 0;
        if (r !== l) r[i] = 0;
        if (this.started) this.starve();
        continue;
      }

      const idx  = Math.floor(this.tail);
      const frac = this.tail - idx;
      const a = idx % this.size;
      const b = (idx + 1) % this.size;

      l[i] = this.left[a] + (this.left[b] - this.left[a]) * frac;
      if (r !== l) r[i] = this.right[a] + (this.right[b] - this.right[a]) * frac;

      this.tail += step;
      const used = Math.floor(this.tail) - idx;
      if (used) this.count -= used;
      if (this.tail >= this.size) this.tail -= this.size;
    }

    // Earn the cushion back, a little at a time.
    if (this.started && this.target > this.minTarget) {
      this.cleanFor += n * this.ratio;

      if (this.cleanFor >= this.cleanNeeded) {
        this.cleanFor = 0;
        this.target = Math.max(this.minTarget, this.target - this.shrink);
      }
    }
  }

  // Milliseconds of sound held, which is milliseconds of delay.
  heldMs() {
    return this.count * 1000 / this.srcRate;
  }
};
