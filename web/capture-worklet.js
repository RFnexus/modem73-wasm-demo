class CaptureProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this._buf = new Float32Array(2048);
        this._n = 0;
    }
    process(inputs) {
        const ch = inputs[0] && inputs[0][0];
        if (ch) {
            for (let i = 0; i < ch.length; i++) {
                this._buf[this._n++] = ch[i];
                if (this._n === this._buf.length) {
                    this.port.postMessage(this._buf.slice(0, this._n));
                    this._n = 0;
                }
            }
        }
        return true;  // keep processor alive
    }
}
registerProcessor('capture-processor', CaptureProcessor);
