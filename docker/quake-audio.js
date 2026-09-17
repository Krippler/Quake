//
// The worklet that plays the container's sound, where the browser offers one.
//
// AudioWorklet is gated on a secure context, and this page is normally served
// over plain HTTP from a machine on the network, which is not one. The page
// falls back to a ScriptProcessorNode there; see play.html. Everything either
// of them does is in QuakeRing, so the two cannot drift apart.
//
class QuakeAudio extends AudioWorkletProcessor {
  constructor(options) {
    const o = options.processorOptions;
    super();
    this.ring = new QuakeRing(o.srcRate, sampleRate,
                             o.targetMs, o.maxTargetMs, o.maxMs);
    this.port.onmessage = (e) => this.ring.push(e.data);
  }

  process(inputs, outputs) {
    const out = outputs[0];
    this.ring.read(out[0], out.length > 1 ? out[1] : out[0]);
    return true;
  }
}

registerProcessor('quake-audio', QuakeAudio);
