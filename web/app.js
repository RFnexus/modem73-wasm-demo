'use strict';

const SAMPLE_RATE = 48000;
const FLUSH_SILENCE = 60000;  
const LEAD_SILENCE = 4800;

let Module = null;     
let modem = null;      
let audioCtx = null;   
let micNode = null;   
let micStream = null;  

const $ = (id) => document.getElementById(id);


function logFrame(text, snr, source, ber, modBits) {
  const div = document.createElement('div');
  const meta = document.createElement('small');
  let m = `[${source}] SNR ${snr.toFixed(1)} dB`;
  if (typeof ber === 'number' && ber >= 0) m += ` · BER ${(ber * 100).toFixed(2)}%`;
  if (modBits) m += ` · ${modBits} bit/sym`;
  m += ` · ${text.length} chars: `;
  meta.textContent = m;
  const span = document.createElement('span');
  span.textContent = text;
  div.append(meta, span);
  $('out').appendChild(div);
}

function logFrames(frames, source) {
  for (const f of frames) routeRxFrame(f, source);
  updateStats();
}
function note(msg) { $('status').textContent = msg; }


const MSG_PREFIX = 'M73:';
const PING_PREFIX = 'PING:';

const buildChatPayload = (callsign, text) => MSG_PREFIX + callsign + ':' + text;

function parseChat(text) {
  const bytes = new TextEncoder().encode(text);
  if (bytes.length <= 4 ||
      bytes[0] !== 0x4d || bytes[1] !== 0x37 || bytes[2] !== 0x33 || bytes[3] !== 0x3a)
    return null;
  const sep = bytes.indexOf(0x3a, 4);
  if (sep < 0 || sep > 16) return null;
  if (bytes.length - (sep + 1) > 200) return null;
  const dec = new TextDecoder();
  let from = dec.decode(bytes.slice(4, sep));
  let body = dec.decode(bytes.slice(sep + 1));
  from = Array.from(from, (c) => (c >= ' ' && c <= '~') ? c : '?').join('');
  body = Array.from(body, (c) => c.charCodeAt(0) < 32 ? ' ' : c).join('');
  return { from, text: body };
}

function parsePing(text) {
  if (!text.startsWith(PING_PREFIX)) return null;
  return { from: text.slice(PING_PREFIX.length) };
}

function logChat(from, text, outgoing, source) {
  const div = document.createElement('div');
  const meta = document.createElement('small');
  const time = new Date().toLocaleTimeString();
  meta.textContent =
    `${time} ${outgoing ? '→' : '←'} ${from}` +
    (source ? ` [${source}]` : '') + ': ';
  const span = document.createElement('span');
  span.textContent = text;
  div.append(meta, span);
  $('out').appendChild(div);
}

function routeRxFrame(f, source) {
  const chat = parseChat(f.text);
  if (chat) { logChat(chat.from, chat.text, false, source); return; }
  const ping = parsePing(f.text);
  if (ping) { logChat(ping.from, '(ping)', false, source); return; }
  logFrame(f.text, f.snr, source, f.ber, f.modBits);
}

const callsignValue = () => ($('callsign').value || 'WASM').trim();

function generateTone(freqHz, ms, amp = 0.8) {
  const n = Math.max(0, Math.round(ms / 1000 * SAMPLE_RATE));
  const out = new Float32Array(n);
  const w = 2 * Math.PI * freqHz / SAMPLE_RATE;
  for (let i = 0; i < n; i++) out[i] = amp * Math.sin(w * i);
  return out;
}

function applyVox(pcm) {
  if (!$('voxenable').checked) return pcm;
  const lead = generateTone(parseFloat($('voxleadfreq').value) || 0, parseFloat($('voxleadms').value) || 0);
  const tail = generateTone(parseFloat($('voxtailfreq').value) || 0, parseFloat($('voxtailms').value) || 0);
  const out = new Float32Array(lead.length + pcm.length + tail.length);
  out.set(lead, 0);
  out.set(pcm, lead.length);
  out.set(tail, lead.length + pcm.length);
  return out;
}


function syncFamilyUI() {
  const mfsk = $('family').value === 'mfsk';
  $('ofdm-fields').hidden = mfsk;
  $('mfsk-fields').hidden = !mfsk;
}

function applyConfig() {
  const cf = parseInt($('cfreq').value, 10) || 1500;
  if ($('family').value === 'mfsk') {
    const rc = modem.configureMfsk(cf, parseInt($('mfsk').value, 10));
    if (rc !== 0) { $('cfginfo').textContent = 'invalid MFSK mode'; return false; }
    const name = $('mfsk').selectedOptions[0].textContent.split(' ')[0];
    $('cfginfo').textContent = `max payload: ${modem.payloadSize()} bytes/frame · ${name}`;
    updateModeInfo();
    return true;
  }
  const rc = modem.configure(
    $('callsign').value.trim() || 'WASM', cf,
    $('mod').value, $('rate').value, parseInt($('frame').value, 10));
  if (rc === -1) { $('cfginfo').textContent = 'invalid callsign (A-Z 0-9 / space only)'; return false; }
  if (rc === -2) { $('cfginfo').textContent = 'invalid modulation/rate/frame combo'; return false; }
  $('cfginfo').textContent = `max payload: ${modem.payloadSize()} bytes/frame · ${$('mod').value} ${$('rate').value}`;
  updateModeInfo();
  return true;
}


function ctx() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: SAMPLE_RATE });
  if (audioCtx.state === 'suspended') audioCtx.resume();
  return audioCtx;
}


function encodeMessage() {
  if (!applyConfig()) return null;
  const text = $('rawpacket').checked
    ? $('msg').value
    : buildChatPayload(callsignValue(), $('msg').value);
  const max = modem.payloadSize();
  const nbytes = new TextEncoder().encode(text).length;
  if (nbytes > max)
    note(`message ${nbytes} B > ${max} B/frame`);
  const pcm = modem.encode(text);
  const nframes = Math.max(1, Math.ceil(nbytes / Math.max(1, max)));
  $('txinfo').textContent =
    `${pcm.length} samples · ${(pcm.length / SAMPLE_RATE).toFixed(2)} s` +
    (nframes > 1 ? ` · ${nframes} frames` : '');
  return pcm;
}

function loopback() {
  const pcm = encodeMessage();
  if (!pcm) return;
  

  const stream = new Float32Array(LEAD_SILENCE + pcm.length + FLUSH_SILENCE);
  stream.set(pcm, LEAD_SILENCE);
  modem.reset();



  const frames = modem.decode(stream);

  if (frames.length === 0) { note('loopback produced no frame (check config)'); return; }
  note('');

  logFrames(frames, 'loop');
}

function playAudio() {
  const pcm = encodeMessage();
  if (!pcm) return;
  const out = applyVox(pcm);
  const c = ctx();
  const buf = c.createBuffer(1, out.length, SAMPLE_RATE);
  buf.copyToChannel(out, 0);
  const src = c.createBufferSource();
  src.buffer = buf;
  src.connect(c.destination);
  src.start();
  note(`playing ${(out.length / SAMPLE_RATE).toFixed(2)} s of audio…`);
  src.onended = () => note('');
}


function encodeWav(pcm, sampleRate) {
  const n = pcm.length;
  const buffer = new ArrayBuffer(44 + n * 2);
  const view = new DataView(buffer);
  const writeStr = (off, s) => { for (let i = 0; i < s.length; i++) view.setUint8(off + i, s.charCodeAt(i)); };
  writeStr(0, 'RIFF');
  view.setUint32(4, 36 + n * 2, true);
  writeStr(8, 'WAVE');
  writeStr(12, 'fmt ');
  view.setUint32(16, 16, true);            
  view.setUint16(20, 1, true);           
  view.setUint16(22, 1, true);              
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true); 
  view.setUint16(32, 2, true);              
  view.setUint16(34, 16, true);             
  writeStr(36, 'data');
  view.setUint32(40, n * 2, true);
  for (let i = 0, off = 44; i < n; i++, off += 2) {
    const s = Math.max(-1, Math.min(1, pcm[i]));
    view.setInt16(off, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }
  return new Blob([buffer], { type: 'audio/wav' });
}

function exportWav() {
  const pcm = encodeMessage();
  if (!pcm) return;
  const out = applyVox(pcm);
  const blob = encodeWav(out, SAMPLE_RATE);
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `modem73-${$('mod') && $('mod').value || 'audio'}.wav`.toLowerCase();
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(a.href);
  note(`exported ${(out.length / SAMPLE_RATE).toFixed(2)} s WAV (${blob.size} bytes)`);
}


async function fileToPcm48k(file) {
  const arrayBuf = await file.arrayBuffer();
  const decodeCtx = new (window.AudioContext || window.webkitAudioContext)();
  let audioBuf;
  try {
    audioBuf = await decodeCtx.decodeAudioData(arrayBuf);
  } finally {
    decodeCtx.close();
  }
  const targetLen = Math.max(1, Math.ceil(audioBuf.duration * SAMPLE_RATE));
  const off = new OfflineAudioContext(1, targetLen, SAMPLE_RATE);
  const src = off.createBufferSource();
  src.buffer = audioBuf;
  src.connect(off.destination);
  src.start();
  const rendered = await off.startRendering();
  return rendered.getChannelData(0);
}

async function decodeFile(file) {
  if (!file) return;
  $('fileinfo').textContent = `decoding ${file.name} …`;
  if (!applyConfig()) { $('fileinfo').textContent = 'fix config first'; return; }
  let pcm;
  try {
    pcm = await fileToPcm48k(file);
  } catch (err) {
    $('fileinfo').textContent = `could not decode ${file.name}: ${err.message}`;
    return;
  }
  modem.reset();
  const frames = modem.decode(pcm);
  const dur = (pcm.length / SAMPLE_RATE).toFixed(2);
  $('fileinfo').textContent =
    `${file.name}: ${dur} s, ${frames.length} frame(s)`;
  logFrames(frames, 'file');
}

// receive
async function toggleMic() {

  if (micNode) { stopMic(); return; }
  try {
    const c = ctx();
    await c.audioWorklet.addModule('capture-worklet.js');
    micStream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false }
    });
    const srcNode = c.createMediaStreamSource(micStream);
    micNode = new AudioWorkletNode(c, 'capture-processor');
    modem.reset();
    if (!applyConfig()) { stopMic(); return; }


    const analyser = c.createAnalyser();
    analyser.fftSize = 2048;
    analyser.smoothingTimeConstant = 0.4;
    srcNode.connect(analyser);
    startWaterfall(analyser);
    micNode.port.onmessage = (e) => {

      const frames = modem.decode(e.data);
      if (frames.length) logFrames(frames, 'mic');
    };

    srcNode.connect(micNode);




    const sink = c.createGain(); sink.gain.value = 0;
    micNode.connect(sink).connect(c.destination);
    $('mic').textContent = 'Stop microphone';
    note(`listening at ${c.sampleRate} Hz. Decoding is live `);
    if (c.sampleRate !== SAMPLE_RATE)
      note(`warning: AudioContext is ${c.sampleRate} Hz, !`);
  } catch (err) {
    note('microphone error: ' + err.message);
    stopMic();
  }
}

function stopMic() {
  if (micNode) { micNode.disconnect(); micNode = null; }
  if (micStream) { micStream.getTracks().forEach(t => t.stop()); micStream = null; }
  stopWaterfall();
  $('mic').textContent = 'Start microphone';
}



function recommendedBand() {
  if ($('family').value === 'mfsk') return 'HF / VHF';
  return $('mod').selectedIndex <= 2 ? 'HF / VHF' : 'VHF / UHF';
}


function updateModeInfo() {
  try {
    const payload = modem.payloadSize();
    const probe = modem.encode('');
    const secs = probe.length / SAMPLE_RATE;
    const bps = secs > 0 ? Math.round((payload + 2) * 8 / secs) : 0;
    const rate = bps >= 1000 ? `${(bps / 1000).toFixed(2)} kb/s` : `${bps} b/s`;
    $('modeinfo').textContent =
      `payload ${payload} B/frame · ${rate} · ${secs.toFixed(2)} s/frame · band ${recommendedBand()}`;
  } catch (e) {
    $('modeinfo').textContent = '';
  }
}



const LOREM = ('lorem ipsum dolor sit amet consectetur adipiscing elit sed do ' +
  'eiusmod tempor incididunt ut labore et dolore magna aliqua ut enim ad minim ' +
  'veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo ' +
  'consequat duis aute irure dolor in reprehenderit in voluptate velit esse cillum ' +
  'dolore eu fugiat nulla pariatur excepteur sint occaecat cupidatat non proident ' +
  'sunt in culpa qui officia deserunt mollit anim id est laborum').split(' ');

const byteLen = (s) => new TextEncoder().encode(s).length;

function makeLorem(maxBytes) {
  let out = 'Lorem ipsum';
  for (let i = 0; byteLen(out + ' ' + LOREM[i % LOREM.length]) <= maxBytes - 1; i++)
    out += ' ' + LOREM[i % LOREM.length];
  return out + '.';
}


function fillLorem(fillFrame) {
  if (!applyConfig()) return;
  const cap = modem.payloadSize();
  const target = fillFrame ? cap : Math.min(cap, 120);
  $('msg').value = makeLorem(Math.max(16, target));
  $('loreminfo').textContent = `${byteLen($('msg').value)} / ${cap} bytes`;
  encodeMessage();
}



let wfAnalyser = null, wfRAF = 0;

function wfColor(v) {
  const t = v / 255;
  if (t < 0.25) return `rgb(0,0,${Math.round(80 + 175 * (t / 0.25))})`;
  if (t < 0.5)  return `rgb(0,${Math.round(255 * (t - 0.25) / 0.25)},255)`;
  if (t < 0.75) return `rgb(${Math.round(255 * (t - 0.5) / 0.25)},255,${Math.round(255 * (1 - (t - 0.5) / 0.25))})`;
  return `rgb(255,${Math.round(255 * (1 - (t - 0.75) / 0.25))},0)`;
}

function startWaterfall(analyser) {
  wfAnalyser = analyser;
  const cv = $('waterfall');
  const g = cv.getContext('2d');
  const w = cv.width, h = cv.height;
  const bins = analyser.frequencyBinCount;            
  const hzPerBin = (SAMPLE_RATE / 2) / bins;
  const maxHz = 4000;
  const maxBin = Math.max(1, Math.min(bins, Math.round(maxHz / hzPerBin)));
  const data = new Uint8Array(bins);
  const cf = () => parseInt($('cfreq').value, 10) || 1500;

  const draw = () => {
    if (!wfAnalyser) return;
    wfAnalyser.getByteFrequencyData(data);
    g.drawImage(cv, 0, 1);                              
    for (let x = 0; x < w; x++) {
      const bin = Math.min(maxBin - 1, Math.floor(x / w * maxBin));
      g.fillStyle = wfColor(data[bin]);
      g.fillRect(x, 0, 1, 1);
    }

    g.fillStyle = 'rgba(255,40,40,0.9)';
    for (const hz of [cf() - 1200, cf() + 1200]) {
      const x = Math.round(hz / maxHz * w);
      if (x >= 0 && x < w) g.fillRect(x, 0, 1, 1);
    }
    wfRAF = requestAnimationFrame(draw);
  };
  wfRAF = requestAnimationFrame(draw);
}

function stopWaterfall() {
  if (wfRAF) cancelAnimationFrame(wfRAF);
  wfRAF = 0;
  wfAnalyser = null;
}



function updateStats() {
  if (!modem.stats) {                
    $('stats').textContent = 'rebuild WASM (./build.sh) to show BER / decode stats';
    return;
  }
  const s = modem.stats();
  const ema = modem.berEma ? modem.berEma() : -1;
  $('stats').textContent =
    `EMA BER ${ema >= 0 ? (ema * 100).toFixed(2) + '%' : '—'}` +
    ` · syncs ${s.syncCount} · CRC err ${s.crcErrors}` +
    ` · preamble err ${s.preambleErrors} · erased sym ${s.erasedSymbols}`;
}

function resetStats() {
  if (modem.resetStats) modem.resetStats();
  $('stats').textContent = 'stats reset';
}


createModem73().then((mod) => {
  Module = mod;
  modem = new Module.Modem();
  syncFamilyUI();
  applyConfig();
  $('family').onchange = () => { syncFamilyUI(); applyConfig(); };
  $('mfsk').onchange = applyConfig;
  $('apply').onclick = applyConfig;
  $('loopback').onclick = loopback;
  $('play').onclick = playAudio;
  $('export').onclick = exportWav;
  $('lorem').onclick = () => fillLorem(false);
  $('loremfill').onclick = () => fillLorem(true);
  $('mic').onclick = toggleMic;
  $('file').onchange = (e) => { decodeFile(e.target.files[0]); e.target.value = ''; };
  $('resetstats').onclick = resetStats;
  $('clear').onclick = () => { $('out').innerHTML = ''; };
  note('');
}).catch((e) => note('failed to load WASM module: ' + e));
