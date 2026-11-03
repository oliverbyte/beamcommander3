<template>
  <section class="mtb" :class="{ editing }">
    <header class="mtb-bar">
      <div class="mtb-bar-left">
        <button class="mtb-btn mode" :class="{ active: editing }" :aria-pressed="editing" @click="editing = !editing">
          {{ editing ? 'Edit mode' : 'Run mode' }}
        </button>
        <template v-if="editing">
          <button class="mtb-btn" @click="addWidget('fader')">+ Fader</button>
          <button class="mtb-btn" @click="addWidget('button')">+ Button</button>
          <button class="mtb-btn" @click="addWidget('knob')">+ Knob</button>
        </template>
      </div>
      <p class="mtb-hint">{{ editing ? 'Drag to move, drag the corner to resize, select to configure.' : 'Touch the controls to operate them.' }}</p>
    </header>

    <div class="mtb-body">
      <div ref="canvasEl" class="mtb-canvas" @pointerdown.self="selectedId = null">
        <p v-if="!widgets.length" class="mtb-empty">
          {{ editing ? 'Add an element to get started.' : 'Nothing here yet. Switch to edit mode to add controls.' }}
        </p>

        <div
          v-for="w in widgets" :key="w.id"
          class="mtb-widget"
          :class="{ selected: editing && selectedId === w.id }"
          :style="{ left: w.x + 'px', top: w.y + 'px', width: w.w + 'px', height: w.h + 'px' }"
          @pointerdown="onWidgetDown($event, w)"
        >
          <template v-if="w.type === 'fader'">
            <div class="mtb-label">{{ w.label }}</div>
            <div
              class="mtb-fader"
              @pointerdown="onFaderDown($event, w)"
              @pointermove="onFaderMove($event, w)"
              @pointerup="onFaderUp"
              @pointercancel="onFaderUp"
            >
              <div class="mtb-track">
                <div class="mtb-fill" :style="{ height: percent(w) + '%' }"></div>
                <div class="mtb-thumb" :style="{ bottom: `calc(${percent(w)}% - 10px)` }"></div>
              </div>
            </div>
            <div class="mtb-value">{{ display(w) }}</div>
          </template>

          <template v-else-if="w.type === 'knob'">
            <div class="mtb-label">{{ w.label }}</div>
            <div
              class="mtb-knob"
              @pointerdown="onKnobDown($event, w)"
              @pointermove="onKnobMove($event, w)"
              @pointerup="onKnobUp"
              @pointercancel="onKnobUp"
            >
              <svg viewBox="0 0 100 100" preserveAspectRatio="xMidYMid meet">
                <circle cx="50" cy="50" r="38" class="knob-body" />
                <path d="M 22.1 77.9 A 39.5 39.5 0 1 1 77.9 77.9" class="knob-track" pathLength="100" />
                <path d="M 22.1 77.9 A 39.5 39.5 0 1 1 77.9 77.9" class="knob-arc" pathLength="100" :stroke-dasharray="`${percent(w) * 0.75 + 0.01} 200`" />
                <line x1="50" y1="50" x2="50" y2="18" class="knob-pointer" :transform="`rotate(${-135 + percent(w) * 2.7} 50 50)`" />
              </svg>
            </div>
            <div class="mtb-value">{{ display(w) }}</div>
          </template>

          <template v-else>
            <button
              class="mtb-pad"
              :class="{ down: isLit(w), swatch: defFor(w).swatch, cue: defFor(w).cuePad, populated: defFor(w).cuePad && cues[w.param] }"
              :style="defFor(w).swatch ? { '--swatch': defFor(w).swatch } : null"
              :disabled="editing"
              @pointerdown.prevent="onButtonDown($event, w)"
              @pointerup="onButtonUp(w)"
              @pointercancel="onButtonUp(w)"
              @lostpointercapture="onButtonUp(w)"
            >
              <CuePreview v-if="defFor(w).cuePad && cues[w.param]" :cue="cues[w.param]" />
              <span v-if="defFor(w).icon" class="mtb-pad-icon" aria-hidden="true" v-html="defFor(w).icon"></span>
              <span :class="{ 'mtb-pad-text-small': defFor(w).icon }">{{ w.label }}</span>
            </button>
          </template>

          <div v-if="editing" class="mtb-resize" @pointerdown.stop="onResizeDown($event, w)"></div>
        </div>
        <template v-for="(g, i) in guides" :key="i">
          <div v-if="g.x !== undefined" class="mtb-guide v" :style="{ left: g.x + 'px' }"></div>
          <div v-else class="mtb-guide h" :style="{ top: g.y + 'px' }"></div>
        </template>
      </div>

      <aside v-if="editing" class="mtb-inspector" aria-label="Element settings">
        <template v-if="selected">
          <h2>{{ TYPE_NAMES[selected.type] }}</h2>
          <label>Label
            <input v-model="selected.label" type="text" maxlength="24" />
          </label>
          <label>Function
            <select :value="selected.fn" @change="onFunctionChange(selected, $event.target.value)">
              <optgroup v-for="group in groupedFunctions(selected.type)" :key="group.name" :label="group.name">
                <option v-for="item in group.items" :key="item.id" :value="item.id">{{ item.label }}</option>
              </optgroup>
            </select>
          </label>
          <label v-if="defFor(selected).param">{{ defFor(selected).param.label }}
            <input type="number" :min="defFor(selected).param.min" :max="defFor(selected).param.max" :value="selected.param" @change="onParamChange(selected, $event.target.value)" />
          </label>
          <button class="mtb-btn danger" @click="removeSelected">Delete element</button>
        </template>
        <template v-else>
          <h2>Page</h2>
          <label>Name
            <input v-model="activePage.name" type="text" maxlength="20" />
          </label>
          <div class="mtb-row">
            <button class="mtb-btn" :disabled="pages[0].id === activePage.id" @click="movePage(-1)">Move left</button>
            <button class="mtb-btn" :disabled="pages[pages.length - 1].id === activePage.id" @click="movePage(1)">Move right</button>
          </div>
          <button class="mtb-btn danger" :disabled="pages.length < 2" @click="removePage">Delete page</button>
          <p class="mtb-muted">Select an element to edit it.</p>
          <button class="mtb-btn" @click="resetLayout">Restore default layout</button>
        </template>
      </aside>
    </div>

    <nav class="mtb-pages" aria-label="Pages">
      <button
        v-for="page in pages" :key="page.id"
        class="mtb-page-tab"
        :class="{ active: page.id === activePage.id }"
        @click="selectPage(page.id)"
      >{{ page.name }}</button>
      <button v-if="editing" class="mtb-page-tab add" aria-label="Add page" @click="addPage">+</button>
    </nav>
  </section>
</template>

<script setup>
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import {
  flashPress, flashRelease, cues, isCueCurrent, laserState, lasers, markLocalChange, recallCue, resetState,
  setBrightnessGate, updateLaser, updateState,
} from '../composables/useLaserSocket.js'
import CuePreview from './CuePreview.vue'
import { MOVE_ICONS, SHAPE_ICONS } from './magicIcons.js'
import { buildDefaultPages, colorFnId, COLORS, MOVES, SHAPES } from './magicLayout.js'

const STORAGE_KEY = 'bc3.magicTouch.layout.v3'
const MIN_SIZE = { fader: [70, 140], knob: [80, 100], button: [70, 50] }
const DEFAULT_SIZE = { fader: [110, 280], knob: [120, 150], button: [120, 80] }
const DEFAULT_FN = { fader: 'master_brightness', knob: 'release_time', button: 'flash' }
const TYPE_NAMES = { fader: 'Fader', knob: 'Knob', button: 'Button' }
const CUE_COUNT = 128
const LASER_SLOTS = 8

// Value functions (fader/knob) write a backend field via POST /api/state.
const num = (group, label, key, min, max, decimals = 2, suffix = '') => ({
  group, label, key, min, max,
  format: (v) => `${key === 'intensity' ? Math.round(v * 100) : v.toFixed(decimals)}${key === 'intensity' ? '%' : suffix}`,
})
const VALUE_FNS = {
  master_brightness: num('Master', 'Master brightness', 'intensity', 0, 1),
  release_time: num('Master', 'Release time', 'flash_release_ms', 0, 2000, 0, ' ms'),
  color_r: num('Color', 'Red', 'r', 0, 1),
  color_g: num('Color', 'Green', 'g', 0, 1),
  color_b: num('Color', 'Blue', 'b', 0, 1),
  rainbow_amount: num('Color', 'Rainbow amount', 'rainbow_amount', 0, 1),
  rainbow_speed: num('Color', 'Rainbow speed', 'rainbow_speed', 0, 4, 1),
  radius: num('Shapes', 'Radius', 'radius', 0.05, 1),
  shape_scale: num('Shapes', 'Shape scale', 'shape_scale', -1, 1),
  points: num('Shapes', 'Points', 'points', 8, 1000, 0),
  wave_frequency: num('Shapes', 'Wave frequency', 'wave_frequency', 0.1, 8, 1),
  wave_amplitude: num('Shapes', 'Wave amplitude', 'wave_amplitude', 0, 1),
  wave_speed: num('Shapes', 'Wave speed', 'wave_speed', -4, 4, 1),
  move_speed: num('Move', 'Move speed', 'move_speed', 0, 4),
  move_size: num('Move', 'Move size', 'move_size', 0, 6),
  move_phase: num('Move', 'Phase step per laser', 'move_phase_degrees', 0, 360, 0, '°'),
  pos_x: num('Move', 'Position X', 'pos_x', -1, 1),
  pos_y: num('Move', 'Position Y', 'pos_y', -1, 1),
  rotation: num('Move', 'Rotation', 'rotation_speed', -4, 4, 1, ' rot/s'),
  dot_amount: num('Beam', 'Dot density', 'dot_amount', 0.01, 1),
  flicker: num('Beam', 'Flicker', 'flicker_hz', 0, 30, 1, ' Hz'),
  max_rate: num('Beam', 'Maximum rate', 'max_rate_kpps', 1, 100, 0, ' kpps'),
  rate: {
    group: 'Beam', label: 'Scan rate', key: 'rate_kpps', min: 0, max: 1,
    format: (v) => `${v.toFixed(1)} kpps`,
    // Cubic taper relative to the maximum rate, same as the Beam page.
    toRatio: (v) => Math.cbrt(v / (laserState.max_rate_kpps || 1)),
    fromRatio: (r) => Math.pow(r, 3) * (laserState.max_rate_kpps || 1),
  },
}

// Button functions: 'momentary' = press on touch down, release on up; 'trigger' = action on touch down only.
const hexOf = () => {
  const h = (v) => Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16).padStart(2, '0')
  return `#${h(laserState.r)}${h(laserState.g)}${h(laserState.b)}`
}
const BUTTON_FNS = {
  flash: { group: 'Master', label: 'White flash', mode: 'momentary', press: () => flashPress(), release: () => flashRelease() },
  // Footswitch behavior: gate open while held, fades out over the release time.
  master_brightness_flash: { group: 'Master', label: 'Master brightness flash', mode: 'momentary', press: () => setBrightnessGate(true), release: () => setBrightnessGate(false) },
  blackout_toggle: { group: 'Master', label: 'Blackout toggle', mode: 'trigger', press: () => updateState({ blackout: !laserState.blackout }), active: () => laserState.blackout },
  gate_toggle: { group: 'Master', label: 'Output gate toggle', mode: 'trigger', press: () => setBrightnessGate(!laserState.brightness_gate_open), active: () => laserState.brightness_gate_open },
  reset: { group: 'Master', label: 'Reset settings', mode: 'trigger', press: () => resetState() },
  cue_recall: { group: 'Cues', label: 'Recall cue', mode: 'trigger', cuePad: true, param: { label: 'Cue number', min: 1, max: CUE_COUNT, default: 1 }, press: (w) => recallCue(w.param), active: (w) => isCueCurrent(w.param) },
}
for (const [name, color] of COLORS) {
  const r = parseInt(color.slice(1, 3), 16) / 255
  const g = parseInt(color.slice(3, 5), 16) / 255
  const b = parseInt(color.slice(5, 7), 16) / 255
  BUTTON_FNS[colorFnId(name)] = {
    group: 'Color', label: `Color: ${name}`, swatch: color, mode: 'trigger',
    press: () => updateState({ r, g, b, rainbow_amount: 0 }),
    active: () => hexOf() === color && laserState.rainbow_amount === 0,
  }
}
for (const shape of SHAPES) {
  BUTTON_FNS[`shape_${shape}`] = { group: 'Shapes', label: `Shape: ${shape}`, icon: SHAPE_ICONS[shape], mode: 'trigger', press: () => updateState({ shape }), active: () => laserState.shape === shape }
}
for (const mode of MOVES) {
  BUTTON_FNS[`move_${mode}`] = { group: 'Move', label: `Move: ${mode}`, icon: MOVE_ICONS[mode], mode: 'trigger', press: () => updateState({ move_mode: mode }), active: () => laserState.move_mode === mode }
}
for (let n = 1; n <= LASER_SLOTS; n++) {
  BUTTON_FNS[`laser_arm_${n}`] = {
    group: 'Lasers', label: `Laser ${n} arm toggle`, mode: 'trigger',
    press: () => { const l = lasers[n - 1]; return l ? updateLaser(l.id, { armed: !l.armed }) : Promise.resolve() },
    active: () => !!lasers[n - 1]?.armed,
  }
}

const FUNCTIONS = { fader: VALUE_FNS, knob: VALUE_FNS, button: BUTTON_FNS }
const defFor = (w) => FUNCTIONS[w.type]?.[w.fn]
function groupedFunctions(type) {
  const groups = new Map()
  for (const [id, def] of Object.entries(FUNCTIONS[type])) {
    if (!groups.has(def.group)) groups.set(def.group, [])
    groups.get(def.group).push({ id, label: def.label })
  }
  return [...groups].map(([name, items]) => ({ name, items }))
}
const defaultLabel = (w) => {
  const def = defFor(w)
  return def.param ? `${def.label} ${w.param}` : def.label
}

const newId = () => `w${Date.now().toString(36)}${Math.random().toString(36).slice(2, 5)}`

function materialize(spec) {
  const widget = { ...spec, id: newId() }
  widget.label ??= defaultLabel(widget)
  return widget
}
function cleanWidgets(list) {
  return (Array.isArray(list) ? list : []).filter((w) => w && defFor(w))
}
function defaultPages() {
  return buildDefaultPages().map((p) => ({ id: newId(), name: p.name, widgets: p.widgets.map(materialize) }))
}
function loadPages() {
  try {
    const stored = JSON.parse(localStorage.getItem(STORAGE_KEY) || 'null')
    if (Array.isArray(stored?.pages) && stored.pages.length) {
      return stored.pages.map((p) => ({ id: p.id || newId(), name: String(p.name || 'Page'), widgets: cleanWidgets(p.widgets) }))
    }
  } catch { /* fall through to defaults */ }
  return defaultPages()
}

const pages = ref(loadPages())
const activePageId = ref(pages.value[0].id)
const activePage = computed(() => pages.value.find((p) => p.id === activePageId.value) || pages.value[0])
const widgets = computed({
  get: () => activePage.value.widgets,
  set: (list) => { activePage.value.widgets = list },
})
const editing = ref(false)
const selectedId = ref(null)
const heldId = ref(null)
const canvasEl = ref(null)
const selected = computed(() => widgets.value.find((w) => w.id === selectedId.value) || null)

watch(pages, (list) => localStorage.setItem(STORAGE_KEY, JSON.stringify({ pages: list })), { deep: true })
watch(editing, (on) => {
  if (on) releaseHeldButton()
  else selectedId.value = null
})

function selectPage(id) {
  releaseHeldButton()
  selectedId.value = null
  activePageId.value = id
}
function addPage() {
  const page = { id: newId(), name: `Page ${pages.value.length + 1}`, widgets: [] }
  pages.value.push(page)
  selectPage(page.id)
}
function removePage() {
  if (pages.value.length < 2 || !window.confirm(`Delete page "${activePage.value.name}"?`)) return
  const index = pages.value.findIndex((p) => p.id === activePageId.value)
  pages.value.splice(index, 1)
  selectPage(pages.value[Math.max(0, index - 1)].id)
}
function movePage(delta) {
  const index = pages.value.findIndex((p) => p.id === activePageId.value)
  const target = index + delta
  if (target < 0 || target >= pages.value.length) return
  const [page] = pages.value.splice(index, 1)
  pages.value.splice(target, 0, page)
}
function resetLayout() {
  if (!window.confirm('Replace all pages with the default live layout? Your own layout will be lost.')) return
  releaseHeldButton()
  pages.value = defaultPages()
  selectPage(pages.value[0].id)
}

function addWidget(type) {
  const offset = (widgets.value.length % 6) * 24
  const fn = DEFAULT_FN[type]
  const [w, h] = DEFAULT_SIZE[type]
  const widget = materialize({ type, fn, x: 24 + offset, y: 24 + offset, w, h })
  widgets.value.push(widget)
  selectedId.value = widget.id
}
function removeSelected() {
  widgets.value = widgets.value.filter((w) => w.id !== selectedId.value)
  selectedId.value = null
}
function onFunctionChange(w, fn) {
  const previousLabel = defaultLabel(w)
  w.fn = fn
  const param = defFor(w).param
  if (param) w.param = param.default
  else delete w.param
  if (w.label === previousLabel) w.label = defaultLabel(w)
}
function onParamChange(w, value) {
  const param = defFor(w).param
  const previousLabel = defaultLabel(w)
  w.param = Math.max(param.min, Math.min(param.max, Math.round(Number(value)) || param.default))
  if (w.label === previousLabel) w.label = defaultLabel(w)
}

function ratio(w) {
  const def = defFor(w)
  const v = Number(laserState[def.key]) || 0
  if (def.toRatio) return Math.max(0, Math.min(1, def.toRatio(v)))
  return Math.max(0, Math.min(1, (v - def.min) / (def.max - def.min)))
}
const percent = (w) => ratio(w) * 100
const display = (w) => defFor(w).format(Number(laserState[defFor(w).key]) || 0)

// Throttled send of the latest value over REST; markLocalChange guards against the status poll overwriting it.
let pending = {}
let timer = null
function sendValue(key, value) {
  markLocalChange()
  laserState[key] = value
  pending[key] = value
  if (timer) return
  timer = setTimeout(() => {
    const update = pending
    pending = {}
    timer = null
    updateState(update).catch(console.error)
  }, 70)
}
function sendRatio(w, r) {
  const def = defFor(w)
  const clamped = Math.max(0, Math.min(1, r))
  sendValue(def.key, def.fromRatio ? def.fromRatio(clamped) : def.min + clamped * (def.max - def.min))
}

let drag = null // { kind: 'move' | 'resize', id, startX, startY, orig }
const guides = ref([])
const GRID = 24
const SNAP = 8

// Snaps one axis: an edge/center of the dragged box aligns to other elements first, else its start edge snaps to the grid.
// `fractions` picks which points of the box (0 = start, 0.5 = center, 1 = end) take part in alignment.
function snapAxis(pos, size, others, fractions) {
  let best = null
  for (const f of fractions) {
    const point = pos + size * f
    for (const [start, length] of others) {
      for (const target of [start, start + length / 2, start + length]) {
        const diff = target - point
        if (Math.abs(diff) <= SNAP && (!best || Math.abs(diff) < Math.abs(best.diff))) best = { diff, target }
      }
    }
  }
  if (best) return { pos: Math.round(pos + best.diff), guides: [best.target] }
  return { pos: Math.round(pos / GRID) * GRID, guides: [] }
}

function onWidgetDown(event, w) {
  if (!editing.value) return
  selectedId.value = w.id
  drag = { kind: 'move', id: w.id, startX: event.clientX, startY: event.clientY, orig: { x: w.x, y: w.y } }
  attachDrag()
}
function onResizeDown(event, w) {
  selectedId.value = w.id
  drag = { kind: 'resize', id: w.id, startX: event.clientX, startY: event.clientY, orig: { w: w.w, h: w.h } }
  attachDrag()
}
function attachDrag() {
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', endDrag)
  window.addEventListener('pointercancel', endDrag)
}
function endDrag() {
  drag = null
  guides.value = []
  window.removeEventListener('pointermove', onDragMove)
  window.removeEventListener('pointerup', endDrag)
  window.removeEventListener('pointercancel', endDrag)
}
function onDragMove(event) {
  if (!drag) return
  const w = widgets.value.find((item) => item.id === drag.id)
  if (!w) return
  const dx = event.clientX - drag.startX
  const dy = event.clientY - drag.startY
  const others = widgets.value.filter((o) => o.id !== w.id)
  if (drag.kind === 'move') {
    const sx = snapAxis(drag.orig.x + dx, w.w, others.map((o) => [o.x, o.w]), [0, 0.5, 1])
    const sy = snapAxis(drag.orig.y + dy, w.h, others.map((o) => [o.y, o.h]), [0, 0.5, 1])
    w.x = Math.max(0, sx.pos)
    w.y = Math.max(0, sy.pos)
    guides.value = [...sx.guides.map((x) => ({ x })), ...sy.guides.map((y) => ({ y }))]
  } else {
    const [minW, minH] = MIN_SIZE[w.type]
    const sx = snapAxis(w.x + Math.max(minW, drag.orig.w + dx), 0, others.map((o) => [o.x, o.w]), [0])
    const sy = snapAxis(w.y + Math.max(minH, drag.orig.h + dy), 0, others.map((o) => [o.y, o.h]), [0])
    w.w = Math.max(minW, sx.pos - w.x)
    w.h = Math.max(minH, sy.pos - w.y)
    guides.value = [...sx.guides.map((x) => ({ x })), ...sy.guides.map((y) => ({ y }))]
  }
}

// Fader: absolute position along the track.
let faderActive = false
function setFromPointer(event, w) {
  const rect = event.currentTarget.getBoundingClientRect()
  sendRatio(w, 1 - (event.clientY - rect.top) / rect.height)
}
function onFaderDown(event, w) {
  if (editing.value) return
  event.stopPropagation()
  faderActive = true
  event.currentTarget.setPointerCapture(event.pointerId)
  setFromPointer(event, w)
}
function onFaderMove(event, w) { if (faderActive) setFromPointer(event, w) }
function onFaderUp() { faderActive = false }

// Knob: relative vertical drag, full range over 200px.
let knobDrag = null
function onKnobDown(event, w) {
  if (editing.value) return
  event.stopPropagation()
  knobDrag = { startY: event.clientY, startRatio: ratio(w) }
  event.currentTarget.setPointerCapture(event.pointerId)
}
function onKnobMove(event, w) {
  if (knobDrag) sendRatio(w, knobDrag.startRatio + (knobDrag.startY - event.clientY) / 200)
}
function onKnobUp() { knobDrag = null }

// Button: momentary functions stay active while held; trigger functions fire once on touch down.
let heldWidget = null
const isLit = (w) => heldId.value === w.id || !!defFor(w).active?.(w)
function onButtonDown(event, w) {
  if (editing.value || heldId.value) return
  const def = defFor(w)
  if (def.mode === 'trigger') {
    def.press(w)?.catch?.(console.error)
    return
  }
  event.currentTarget.setPointerCapture(event.pointerId)
  heldId.value = w.id
  heldWidget = w
  def.press(w).catch(console.error)
}
function onButtonUp(w) {
  if (heldId.value !== w.id) return
  heldId.value = null
  defFor(w).release(w).catch(console.error)
  heldWidget = null
}
function releaseHeldButton() {
  if (!heldId.value) return
  heldId.value = null
  if (heldWidget && defFor(heldWidget)) defFor(heldWidget).release(heldWidget).catch(console.error)
  heldWidget = null
}

onBeforeUnmount(() => {
  endDrag()
  releaseHeldButton()
  clearTimeout(timer)
  if (Object.keys(pending).length) updateState(pending).catch(console.error)
})
</script>

<style scoped>
.mtb { display: flex; flex-direction: column; height: 100%; min-height: 0; color: #e7f0ed; }
.mtb-bar { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 22px; border-bottom: 1px solid rgba(159,190,187,.16); }
.mtb-bar-left { display: flex; gap: 8px; }
.mtb-hint, .mtb-muted { margin: 0; color: #91a6a2; font-size: 12px; }
.mtb-btn { min-height: 38px; padding: 6px 14px; color: inherit; background: rgba(255,255,255,.04); border: 1px solid rgba(159,190,187,.16); border-radius: 7px; font: inherit; font-size: 13px; cursor: pointer; touch-action: manipulation; }
.mtb-btn.active { color: #eafff3; background: rgba(101,230,165,.13); border-color: #65e6a5; }
.mtb-btn.danger { color: #ffaaa1; border-color: rgba(255,124,111,.45); }
.mtb-body { flex: 1 1 auto; display: flex; min-height: 0; }
.mtb-canvas { position: relative; flex: 1 1 auto; min-width: 0; overflow: auto; touch-action: none; }
.editing .mtb-canvas { background-image: radial-gradient(rgba(159,190,187,.22) 1px, transparent 1px); background-size: 24px 24px; }
.mtb-empty { margin: 24px; color: #91a6a2; font-size: 13px; pointer-events: none; }
.mtb-widget { position: absolute; display: flex; flex-direction: column; box-sizing: border-box; padding: 10px; background: #11191f; border: 1px solid rgba(159,190,187,.16); border-radius: 8px; user-select: none; }
.editing .mtb-widget { cursor: move; border-style: dashed; }
.mtb-widget.selected { border-color: #65e6a5; box-shadow: 0 0 0 2px rgba(101,230,165,.25); }
.mtb-label { font-size: 12px; text-align: center; color: #c7d4d0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mtb-fader, .mtb-knob { flex: 1 1 auto; display: flex; justify-content: center; min-height: 0; padding: 10px 0; touch-action: none; cursor: pointer; }
.editing .mtb-fader, .editing .mtb-knob, .editing .mtb-pad { pointer-events: none; }
.mtb-track { position: relative; width: 12px; height: 100%; background: rgba(255,255,255,.08); border-radius: 999px; }
.mtb-fill { position: absolute; left: 0; right: 0; bottom: 0; background: #65e6a5; border-radius: 999px; }
.mtb-thumb { position: absolute; left: 50%; width: 36px; height: 20px; margin-left: -18px; background: #eafff3; border-radius: 5px; box-shadow: 0 0 0 3px rgba(101,230,165,.25); pointer-events: none; }
.mtb-knob svg { width: 100%; height: 100%; }
.knob-body { fill: #1a252c; stroke: rgba(159,190,187,.25); stroke-width: 1.5; }
.knob-track, .knob-arc { fill: none; stroke-width: 5; stroke-linecap: round; }
.knob-track { stroke: rgba(255,255,255,.08); stroke-dasharray: 75 200; }
.knob-arc { stroke: #65e6a5; }
.knob-pointer { stroke: #eafff3; stroke-width: 4; stroke-linecap: round; }
.mtb-value { text-align: center; font-size: 12px; color: #65e6a5; font-variant-numeric: tabular-nums; }
.mtb-pad { display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px; flex: 1 1 auto; min-height: 0; padding: 4px; color: #fff3cc; background: linear-gradient(135deg, #7d461c, #3e291b); border: 1px solid rgba(255,188,89,.55); border-radius: 8px; font: inherit; font-size: 12px; font-weight: 700; text-transform: uppercase; cursor: pointer; touch-action: none; user-select: none; }
.mtb-pad.down { background: #e59036; color: #17120c; }
.mtb-pad.cue { position: relative; overflow: hidden; align-items: flex-end; justify-content: flex-end; color: #6a7090; background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.15); font-size: 13px; font-weight: 500; text-shadow: 0 0 2px rgba(0,0,0,.9); }
.mtb-pad.cue > span { position: relative; padding: 0 4px 2px; line-height: 1; }
.mtb-pad.cue.populated { background: rgba(20,22,32,.9); border-color: #48e07a; color: #48e07a; }
.mtb-pad.cue.down { border: 3px solid #a8ff60; box-shadow: inset 0 0 0 1px rgba(168,255,96,.65), 0 0 12px rgba(168,255,96,.75); color: #c2ff89; background: rgba(20,22,32,.9); }
.mtb-pad.swatch { color: #fff; text-shadow: 0 1px 4px rgba(0,0,0,.9); text-transform: none; font-size: 11px; background: linear-gradient(180deg, transparent 30%, rgba(0,0,0,.6)), var(--swatch); border: 2px solid transparent; }
.mtb-pad.swatch.down { background: linear-gradient(180deg, transparent 30%, rgba(0,0,0,.6)), var(--swatch); color: #fff; border-color: #fff; box-shadow: 0 0 0 2px #65e6a5; }
.mtb-row { display: flex; gap: 8px; }
.mtb-row .mtb-btn { flex: 1 1 0; padding-inline: 6px; }
.mtb-btn:disabled { opacity: .4; cursor: default; }
.mtb-pages { display: flex; gap: 6px; padding: 8px 16px; overflow-x: auto; border-top: 1px solid rgba(159,190,187,.16); background: rgba(12,17,22,.9); }
.mtb-page-tab { flex: 0 0 auto; min-width: 96px; min-height: 44px; padding: 6px 18px; color: #a7b9b5; background: transparent; border: 1px solid rgba(159,190,187,.16); border-radius: 7px; font: inherit; font-size: 14px; cursor: pointer; touch-action: manipulation; }
.mtb-page-tab.active { color: #eafff3; background: rgba(101,230,165,.13); border-color: #65e6a5; }
.mtb-page-tab.add { min-width: 44px; padding-inline: 12px; }
.mtb-pad-icon { display: grid; place-items: center; flex: 1 1 auto; width: 100%; min-height: 0; }
.mtb-pad-icon :deep(svg) { width: 100%; height: 100%; max-width: 64px; max-height: 64px; stroke: currentColor; stroke-width: 1.5; stroke-linecap: round; stroke-linejoin: round; }
.mtb-pad-text-small { font-size: 9px; font-weight: 500; line-height: 1.1; }
.mtb-guide { position: absolute; pointer-events: none; background: #ff4fd8; }
.mtb-guide.v { top: 0; bottom: 0; width: 1px; }
.mtb-guide.h { left: 0; right: 0; height: 1px; }
.mtb-resize { position: absolute; right: 0; bottom: 0; width: 18px; height: 18px; cursor: nwse-resize; background: linear-gradient(135deg, transparent 50%, #65e6a5 50%); border-bottom-right-radius: 8px; touch-action: none; }
.mtb-inspector { display: flex; flex-direction: column; gap: 12px; flex: 0 0 240px; padding: 16px; background: rgba(17,25,31,.9); border-left: 1px solid rgba(159,190,187,.16); }
.mtb-inspector h2 { margin: 0; font-size: 15px; }
.mtb-inspector label { display: flex; flex-direction: column; gap: 6px; color: #c7d4d0; font-size: 12px; }
.mtb-inspector input, .mtb-inspector select { padding: 8px; color: inherit; background: #0c1116; border: 1px solid rgba(159,190,187,.16); border-radius: 6px; font: inherit; font-size: 13px; }
</style>
