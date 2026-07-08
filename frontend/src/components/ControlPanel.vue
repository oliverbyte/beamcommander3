<template>
  <div id="ui" :class="{ popout }">
    <div class="panel-body">
    <div class="status-row">
      <span class="dot" :class="{ on: laserState.wsConnected }"></span>
      Preview {{ laserState.wsConnected ? 'live' : 'connecting…' }}
    </div>
    <div class="status-row">
      <span class="dot" :class="{ on: connectedLaserCount > 0 }"></span>
      Lasers {{ connectedLaserCount }}/{{ lasers.length }} streaming
    </div>
    <button class="blackout-btn" :class="{ active: laserState.blackout }" @click="push({ blackout: !laserState.blackout })">
      {{ laserState.blackout ? '◼ BLACKOUT' : '◻ Blackout' }}
    </button>
    <button
      class="gate-btn"
      :class="{ active: laserState.brightness_gate_open }"
      title="Master output gate - like holding a footswitch. Must be open (and Blackout off) for any real laser output."
      @click="toggleGate"
    >
      {{ laserState.brightness_gate_open ? '🔓 Output Enabled' : '🔒 Output Gated (click to enable)' }}
    </button>
    <button
      class="flash-btn"
      @pointerdown="onFlashDown"
      @pointerup="onFlashUp"
      @pointerleave="onFlashUp"
      @pointercancel="onFlashUp"
    >⚡ Flash (hold)</button>
    <button class="reset-btn" @click="reset">↺ Reset to defaults</button>

    <hr />
    <h2>Shape</h2>
    <div class="btn-grid">
      <button v-for="s in SHAPES" :key="s" :class="{ active: laserState.shape === s }" @click="push({ shape: s })">{{ s }}</button>
    </div>
    <div v-for="[lbl,key,mn,mx,st] in [['Radius','radius',0.05,1,0.01],['Scale','shape_scale',-1,1,0.01],['Points','points',8,1000,1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Color</h2>
    <div class="row">
      <input type="color" :value="hexColor" @input="onColor" />
      <label>Beam color</label>
    </div>
    <div v-for="[lbl,key,mn,mx,st] in [['Brightness','intensity',0,1,0.01],['Rainbow','rainbow_amount',0,1,0.01],['Rainbow speed','rainbow_speed',0,4,0.1],['Flash release (ms)','flash_release_ms',0,2000,10]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Transform</h2>
    <div v-for="[lbl,key,mn,mx,st] in [['Pos X','pos_x',-1,1,0.01],['Pos Y','pos_y',-1,1,0.01],['Rotation (rot/s)','rotation_speed',-4,4,0.1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Movement</h2>
    <div class="btn-grid">
      <button v-for="m in MOVES" :key="m" :class="{ active: laserState.move_mode === m }" @click="push({ move_mode: m })">{{ m }}</button>
    </div>
    <div v-for="[lbl,key,mn,mx,st] in [['Speed','move_speed',0,4,0.05],['Size','move_size',0,6,0.01]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Wave</h2>
    <div v-for="[lbl,key,mn,mx,st] in [['Frequency','wave_frequency',0.1,8,0.1],['Amplitude','wave_amplitude',0,1,0.01],['Speed','wave_speed',-4,4,0.1]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>FX</h2>
    <div v-for="[lbl,key,mn,mx,st] in [['Dot amount','dot_amount',0.01,1,0.01],['Flicker Hz','flicker_hz',0,30,0.5]]" :key="key">
      <label>{{ lbl }} <span class="val">{{ fmt(laserState[key]) }}</span></label>
      <input type="range" :min="mn" :max="mx" :step="st" :value="laserState[key]" @input="push({[key]:+$event.target.value})" />
    </div>

    <hr />
    <h2>Scan / Preview</h2>
    <label>Rate (kpps) <span class="val">{{ fmt(laserState.rate_kpps) }} / {{ fmt(laserState.max_rate_kpps) }}</span></label>
    <input type="range" min="0" max="1" step="0.001" :value="ratePos" @input="onRatePos(+$event.target.value)" />
    <label>Max rate (kpps) <span class="val">{{ fmt(laserState.max_rate_kpps) }}</span></label>
    <input type="range" min="1" max="100" step="1" :value="laserState.max_rate_kpps" @input="push({ max_rate_kpps: +$event.target.value })" />
    <label>Eye persistence (ms) <span class="val">{{ persistenceMs }}</span></label>
    <input type="range" min="0" max="100" step="1" :value="persistenceMs" @input="onPersist(+$event.target.value)" />

    <hr />
    <p class="hint">Manage laser (DAC) connections in the Lasers panel.</p>
    <p class="error" v-if="laserState.error">{{ laserState.error }}</p>
    </div>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { laserState, lasers, updateState, resetState, markLocalChange, flashPress, flashRelease, setBrightnessGate } from '../composables/useLaserSocket.js'

const { popout } = defineProps({ popout: { type: Boolean, default: false } })
const emit = defineEmits(['update:persistence'])

const SHAPES = ['circle','line','triangle','square','wave','staticwave']
const MOVES  = ['none','circle','pan','tilt','eight','random']
const persistenceMs = ref(5)

const connectedLaserCount = computed(() => lasers.filter(l => l.connected).length)

const hexColor = computed(() => {
  const to = v => Math.round(v*255).toString(16).padStart(2,'0')
  return '#' + to(laserState.r) + to(laserState.g) + to(laserState.b)
})
function onColor(e) {
  const h = e.target.value
  push({ r: parseInt(h.slice(1,3),16)/255, g: parseInt(h.slice(3,5),16)/255, b: parseInt(h.slice(5,7),16)/255 })
}

// Scan-rate fader: much finer control at the low end (needed to dial in
// strobe-like slow-scan effects), rougher at the high end - same
// exponent (3) and shape as the MIDI "rate_kpps" binding's gamma in
// midi_map.json, so the on-screen slider, MIDI knob and REST all agree on
// what "position" means. `laserState.rate_kpps` itself stays the real,
// linear kpps value (as sent/received over REST and saved in cues) -
// only the on-screen slider's *position* is curved; pos^RATE_GAMMA*max
// gives the kpps value, and the inverse turns a live kpps value (e.g.
// from a cue recall or MIDI move) back into the matching slider position.
const RATE_GAMMA = 3
const ratePos = computed(() => {
  const max = laserState.max_rate_kpps || 1
  return Math.pow(Math.min(1, Math.max(0, laserState.rate_kpps / max)), 1 / RATE_GAMMA)
})
function onRatePos(pos) {
  const max = laserState.max_rate_kpps || 1
  push({ rate_kpps: Math.pow(pos, RATE_GAMMA) * max })
}

// pointerdown/up (not click) so the flash lasts exactly as long as the
// button is actually held - pointerleave/cancel too, so dragging off the
// button (or losing the pointer) still releases it instead of latching on.
let flashHeld = false
function onFlashDown() {
  if (flashHeld) return
  flashHeld = true
  flashPress().catch(console.error)
}
function onFlashUp() {
  if (!flashHeld) return
  flashHeld = false
  flashRelease().catch(console.error)
}
function toggleGate() {
  setBrightnessGate(!laserState.brightness_gate_open).catch(console.error)
}
function fmt(v) { return typeof v === 'boolean' ? String(v) : Number(v).toFixed(2) }
function onPersist(v) { persistenceMs.value = v; emit('update:persistence', v) }

let debounce = null
function push(partial) {
  markLocalChange()
  Object.assign(laserState, partial)
  clearTimeout(debounce)
  debounce = setTimeout(() => updateState(partial).catch(console.error), 80)
}

async function reset() { await resetState().catch(console.error) }
</script>

<style scoped>
/* top:52px (not 12px) leaves clearance for the standalone "BeamCommander3"
   logo now rendered separately in App.vue, fixed top-left of the main
   screen - this panel used to carry that logo itself as its own <h1>. */
#ui {
  position:fixed; top:52px; left:12px; width:275px;
  background:rgba(8,10,18,0.88); border:1px solid rgba(120,130,200,0.25);
  border-radius:10px; padding:14px 16px; backdrop-filter:blur(6px);
  user-select:none; z-index:10; color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  max-height:calc(100vh - 64px); overflow-y:auto;
}
/* Popped out - either into its own draggable browser window, or embedded
   in a FloatingPanel div (see TouchDock.vue). height:100% (not 100vh) so
   it fills whichever parent it's actually in. */
#ui.popout {
  position:static; width:100%; height:100%; max-height:none;
  border:none; border-radius:0; box-sizing:border-box;
  padding:20px 22px 40px;
}
#ui.popout h2 { font-size:12px; }
#ui.popout label { font-size:14px; }
#ui.popout button { padding:12px 10px; font-size:14px; }
#ui.popout .btn-grid button { padding:14px 4px; font-size:13px; }
#ui.popout input[type="range"] { height:28px; }
#ui.popout input[type="text"] { padding:10px 8px; font-size:14px; }
#ui.popout .blackout-btn, #ui.popout .gate-btn, #ui.popout .flash-btn, #ui.popout .reset-btn { padding:14px; font-size:15px; }
h2 { font-size:10px; letter-spacing:1.5px; text-transform:uppercase; margin:8px 0 4px; color:#9aa0bd; }
label { display:block; font-size:11px; margin:6px 0 1px; color:#9aa0bd; }
.val { color:#cfd3e6; margin-left:4px; }
input[type="range"] { width:100%; accent-color:#48e07a; display:block; }
.row { display:flex; align-items:center; gap:8px; margin-top:6px; }
input[type="color"] { width:38px; height:22px; border:none; background:none; padding:0; cursor:pointer; }
input[type="text"] { flex:1; min-width:0; background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.15); color:#cfd3e6; border-radius:4px; padding:4px 6px; font-size:12px; }
button { background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18); color:#cfd3e6; border-radius:4px; padding:4px 8px; font-size:11px; cursor:pointer; }
button:hover { background:rgba(255,255,255,0.14); }
button:disabled { opacity:0.5; cursor:default; }
button.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }
.btn-stop { border-color:rgba(255,100,100,0.5); color:#ff8080; }
.blackout-btn { width:100%; margin-top:6px; padding:6px; font-size:12px; letter-spacing:1px; }
.gate-btn { width:100%; margin-top:6px; padding:6px; font-size:12px; letter-spacing:0.5px; }
.blackout-btn.active { background:rgba(255,50,50,0.28); border-color:#ff4040; color:#ff8080; }
.flash-btn { width:100%; margin-top:6px; padding:6px; font-size:12px; letter-spacing:1px; user-select:none; touch-action:none; }
.flash-btn:active { background:rgba(255,255,255,0.35); border-color:#ffffff; color:#ffffff; }
.reset-btn { width:100%; margin-top:6px; padding:5px; font-size:11px; }
hr { border:none; border-top:1px solid rgba(255,255,255,0.1); margin:8px 0; }
.status-row { display:flex; align-items:center; gap:6px; font-size:11px; margin-bottom:3px; }
.dot { width:7px; height:7px; border-radius:50%; background:#703030; flex-shrink:0; }
.dot.on { background:#48e07a; }
.error { font-size:11px; color:#ff8080; word-break:break-word; margin-top:4px; }
.hint { font-size:10px; color:#6a7090; margin:0 0 8px; line-height:1.4; }
.btn-grid { display:grid; grid-template-columns:repeat(3,1fr); gap:3px; margin-bottom:4px; }
.btn-grid button { padding:4px 2px; font-size:10px; text-align:center; }
</style>
