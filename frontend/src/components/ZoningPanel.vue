<template>
  <div id="zoning-panel" :class="{ popout }">
    <div class="panel-body">
      <p class="hint" v-if="mode === 'zone'">Drag the box to move it, drag an edge to scale that
        axis only. The preview shows each laser exactly as it outputs.</p>
      <p class="hint" v-else>Red boxes are safety masks on this laser's real output
        (where the light lands in the room). Drag to move, drag an edge or
        corner to resize. Light inside is reduced by the chosen percentage.</p>

      <h2>Zone 1 <span class="laser-tag">→ {{ selectedLaser ? selectedLaser.name : '(no lasers)' }}</span></h2>

      <div class="row" v-if="lasers.length">
        <select class="laser-select" v-model.number="selectedId">
          <option v-for="l in lasers" :key="l.id" :value="l.id">
            {{ l.name }}
          </option>
        </select>
      </div>
      <div v-if="selectedLaser" class="laser-status-row" aria-label="Selected laser status">
        <span
          class="status-dot"
          :class="{ on: selectedLaser.connected }"
          :title="selectedLaser.connected ? 'Connected' : 'Not connected'"
          aria-hidden="true"
        ></span>
        <span class="connection-label">{{ selectedLaser.connected ? 'Connected' : 'Not connected' }}</span>
        <span class="arm-status" :class="{ active: selectedLaser.armed }">
          {{ selectedLaser.armed ? '● Armed' : '○ Disarmed' }}
        </span>
      </div>
      <p v-else class="empty">No lasers configured yet - add one in the Lasers panel first.</p>

      <template v-if="selectedLaser">
        <div class="mode-switch" role="tablist" aria-label="Edit">
          <button role="tab" :aria-selected="mode === 'zone'" :class="{ active: mode === 'zone' }" @click="mode = 'zone'">Zone</button>
          <button role="tab" :aria-selected="mode === 'masks'" :class="{ active: mode === 'masks', masks: true }" @click="mode = 'masks'">
            Masks<span v-if="masks.length" class="count">{{ masks.length }}</span>
          </button>
        </div>

        <div
          class="stage"
          :class="'mode-' + mode"
          :style="{ width: CANVAS + 'px', height: CANVAS + 'px' }"
          @pointerdown.self="selectedMask = -1"
        >
          <div class="zone-box" :style="boxStyle" @pointerdown="onBodyDown">
            <div class="handle handle-left"   @pointerdown.stop="onEdgeDown('left', $event)"></div>
            <div class="handle handle-right"  @pointerdown.stop="onEdgeDown('right', $event)"></div>
            <div class="handle handle-top"    @pointerdown.stop="onEdgeDown('top', $event)"></div>
            <div class="handle handle-bottom" @pointerdown.stop="onEdgeDown('bottom', $event)"></div>
          </div>

          <div
            v-for="(m, i) in masks"
            :key="i"
            class="mask-box"
            :class="{ selected: i === selectedMask, lit: m.outline }"
            :style="maskStyle(m)"
            @pointerdown.stop="onMaskDown(i, 'move', $event)"
          >
            <span class="mask-label">{{ i + 1 }} · {{ m.reduce }}%</span>
            <template v-if="i === selectedMask">
              <div
                v-for="h in MASK_HANDLES"
                :key="h"
                class="mhandle"
                :class="'mh-' + h"
                @pointerdown.stop="onMaskDown(i, h, $event)"
              ></div>
            </template>
          </div>
        </div>

        <template v-if="mode === 'zone'">
          <div class="readout">
            <span>X <b>{{ fmt(selectedLaser.zone_x) }}</b></span>
            <span>Y <b>{{ fmt(selectedLaser.zone_y) }}</b></span>
            <span>W <b>{{ fmt(selectedLaser.zone_scale_x) }}</b></span>
            <span>H <b>{{ fmt(selectedLaser.zone_scale_y) }}</b></span>
          </div>

          <button class="reset-btn" @click="reset">↺ Reset zone</button>
        </template>

        <template v-else>
          <div class="mask-toolbar">
            <button class="add-btn" :disabled="masks.length >= MAX_MASKS" @click="addMask">+ Add mask</button>
            <button v-if="anyOutline" class="outline-off-btn" @click="allOutlinesOff">Hide all on laser</button>
          </div>
          <p v-if="!masks.length" class="empty">No masks on this laser yet.</p>

          <div
            v-for="(m, i) in masks"
            :key="'row' + i"
            class="mask-row"
            :class="{ selected: i === selectedMask }"
            @pointerdown="selectedMask = i"
          >
            <div class="mask-row-head">
              <span class="mask-name">Mask {{ i + 1 }}</span>
              <button
                class="outline-btn"
                :class="{ active: m.outline }"
                :aria-pressed="m.outline"
                :title="m.outline ? 'Red outline is being drawn by the laser' : 'Draw this mask\'s border in red on the real laser'"
                @click="setMask(i, { outline: !m.outline }, true)"
              >{{ m.outline ? '● Shown on laser' : '○ Show on laser' }}</button>
              <button class="del-btn" title="Delete mask" @click="removeMask(i)">✕</button>
            </div>
            <label class="reduce-row">
              <span>Reduce</span>
              <input
                type="range" min="0" max="100" step="1"
                :value="m.reduce"
                @input="setMask(i, { reduce: Number($event.target.value) })"
                @change="setMask(i, { reduce: Number($event.target.value) }, true)"
              />
              <b>{{ m.reduce }}%</b>
            </label>
          </div>
          <p v-if="anyOutline" class="hint warn">
            Red outlines are drawn only while the laser is armed and not blacked
            out, and switch off automatically when this panel closes.
          </p>
        </template>
      </template>
    </div>
  </div>
</template>

<script setup>
// Each laser has its own Zone 1 pan/zoom calibration (zone_x/zone_y/
// zone_scale_x/zone_scale_y on its LaserConfig - see laser_daemon.cpp),
// since two projectors in the same rig often need different calibrations
// even though they're all streaming the exact same Zone 1 show content.
// This panel edits *one* laser's calibration at a time, picked from the
// dropdown above. Set purely by dragging - no numeric fader/slider - by
// design: drag the box body to move it, drag an individual edge handle to
// scale just that one axis (independent horizontal/vertical scale). Every
// drag step updates the laser locally and immediately (patchLaserLocal, for
// a responsive box), and pushes to the backend via updateLaser() (debounced
// while dragging, then flushed once more on release) - the backend
// (laser_daemon.cpp) is always the single source of truth, this is never a
// client-only/visual-only setting.
//
// "Masks" mode edits the same laser's safety masks (LaserMask in
// backend/output_pipeline.h): red rectangles in the laser's physical output
// space (the whole stage = the laser's full -1..1 range, *not* relative to
// the zone box) that reduce the light inside them by `reduce` percent. Each
// mask's "Show on laser" draws its border in red on the real laser while
// lining it up; those outlines are switched off again when this panel
// unmounts/closes (and the backend never persists them). Same optimistic
// local patch + debounced push pattern as the zone, via updateLaserMasks().
// Geometry lives in maskGeometry.js (unit-tested).
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { lasers, fetchLasers, updateLaser, updateLaserMasks, patchLaserLocal } from '../composables/useLaserSocket.js'
import { MAX_MASKS, newMask, normalizeMask, moveMask, resizeMask, maskToPx, withoutOutlines } from './maskGeometry.js'

const { popout } = defineProps({ popout: { type: Boolean, default: false } })

const selectedId = ref(null)
const mode = ref('zone') // 'zone' | 'masks' - which layer the stage edits
const selectedMask = ref(-1)
const MASK_HANDLES = ['l', 'r', 't', 'b', 'tl', 'tr', 'bl', 'br']

const selectedLaser = computed(() => lasers.find((l) => l.id === selectedId.value) || null)
const masks = computed(() => selectedLaser.value?.masks || [])
const anyOutline = computed(() => masks.value.some((m) => m.outline))

// Default to the first configured laser as soon as the list loads (or if
// the previously-selected one gets deleted) - never leaves the dropdown on
// a stale/missing id.
watch(
  lasers,
  (list) => {
    if (!list.find((l) => l.id === selectedId.value)) {
      selectedId.value = list.length ? list[0].id : null
    }
  },
  { immediate: true }
)
watch(selectedId, () => { selectedMask.value = -1 })
watch(masks, (list) => { if (selectedMask.value >= list.length) selectedMask.value = list.length - 1 })

onMounted(() => {
  fetchLasers().catch(() => {})
  window.addEventListener('pagehide', allLasersOutlinesOff)
})
// The red outline is a setup aid only: never leave it running on a laser
// once nobody is looking at this panel anymore.
onBeforeUnmount(() => {
  window.removeEventListener('pagehide', allLasersOutlinesOff)
  allLasersOutlinesOff()
})

// The stage square represents the full -1..1 x -1..1 laser range. Bigger
// in popout mode (its own window/floating panel, not squeezed into the
// small fixed corner card) for easier touch/finger dragging - sized to
// comfortably fit inside the 320px-wide FloatingPanel/popup window (see
// TouchDock.vue) with room for the edge handles to spare.
const CANVAS = popout ? 260 : 220
const HALF = CANVAS / 2

function fmt(v) { return Number(v).toFixed(2) }
function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)) }

function toPxX(v) { return HALF + v * HALF }
function toPxY(v) { return HALF - v * HALF } // screen y grows downward; zone_y positive = up

const boxStyle = computed(() => {
  if (!selectedLaser.value) return {}
  const halfW = selectedLaser.value.zone_scale_x * HALF
  const halfH = selectedLaser.value.zone_scale_y * HALF
  const cx = toPxX(selectedLaser.value.zone_x)
  const cy = toPxY(selectedLaser.value.zone_y)
  return {
    left: `${cx - halfW}px`,
    top: `${cy - halfH}px`,
    width: `${halfW * 2}px`,
    height: `${halfH * 2}px`,
  }
})

function maskStyle(m) {
  const px = maskToPx(m, CANVAS)
  return {
    left: `${px.left}px`, top: `${px.top}px`, width: `${px.width}px`, height: `${px.height}px`,
    // fill strength follows the reduction, so a 100% mask reads as "solid"
    '--fill': (0.08 + 0.3 * (m.reduce / 100)).toFixed(3),
  }
}

let debounce = null
function push(partial, flush = false) {
  const id = selectedId.value
  if (id == null) return
  patchLaserLocal(id, partial)
  clearTimeout(debounce)
  if (flush) updateLaser(id, partial).catch(console.error)
  else debounce = setTimeout(() => updateLaser(id, partial).catch(console.error), 80)
}

// ── Masks ───────────────────────────────────────────────────────────────────
let maskDebounce = null
function pushMasks(list, flush = false) {
  const id = selectedId.value
  if (id == null) return
  const clean = list.map(normalizeMask)
  patchLaserLocal(id, { masks: clean })
  clearTimeout(maskDebounce)
  if (flush) updateLaserMasks(id, clean).catch(console.error)
  else maskDebounce = setTimeout(() => updateLaserMasks(id, clean).catch(console.error), 80)
}
function setMask(i, partial, flush = false) {
  pushMasks(masks.value.map((m, k) => (k === i ? { ...m, ...partial } : m)), flush)
}
function addMask() {
  if (masks.value.length >= MAX_MASKS) return
  pushMasks([...masks.value, newMask(masks.value.length)], true)
  selectedMask.value = masks.value.length - 1
  mode.value = 'masks'
}
function removeMask(i) {
  pushMasks(masks.value.filter((_, k) => k !== i), true)
  selectedMask.value = -1
}
function allOutlinesOff() {
  const off = withoutOutlines(masks.value)
  if (off) pushMasks(off, true)
}
function allLasersOutlinesOff() {
  for (const l of lasers) {
    const off = withoutOutlines(l.masks)
    if (off) updateLaserMasks(l.id, off, { keepalive: true }).catch(() => {})
  }
}

// pointerdown/move/up (not drag events) so this works identically with
// mouse and touch - window-level move/up listeners (not just on the
// handle/box itself) so the drag keeps tracking even if the pointer
// outruns the small element while moving fast.
let maskDrag = null
function onMaskDown(i, handle, e) {
  if (mode.value !== 'masks') return
  selectedMask.value = i
  maskDrag = { i, handle, startPx: e.clientX, startPy: e.clientY, start: { ...masks.value[i] } }
  window.addEventListener('pointermove', onMaskMove)
  window.addEventListener('pointerup', onMaskUp)
}
function maskDragResult(e) {
  const dx = (e.clientX - maskDrag.startPx) / HALF
  const dy = -(e.clientY - maskDrag.startPy) / HALF // screen down = output y down
  return maskDrag.handle === 'move'
    ? moveMask(maskDrag.start, dx, dy)
    : resizeMask(maskDrag.start, maskDrag.handle, dx, dy)
}
function onMaskMove(e) {
  if (!maskDrag) return
  const { i } = maskDrag
  const next = maskDragResult(e)
  pushMasks(masks.value.map((m, k) => (k === i ? next : m)))
}
function onMaskUp(e) {
  if (!maskDrag) return
  const { i } = maskDrag
  const next = maskDragResult(e)
  maskDrag = null
  window.removeEventListener('pointermove', onMaskMove)
  window.removeEventListener('pointerup', onMaskUp)
  pushMasks(masks.value.map((m, k) => (k === i ? next : m)), true)
}

// ── Zone ────────────────────────────────────────────────────────────────────
let drag = null
function onBodyDown(e) {
  if (!selectedLaser.value || mode.value !== 'zone') return
  drag = { mode: 'move', startPx: e.clientX, startPy: e.clientY, startX: selectedLaser.value.zone_x, startY: selectedLaser.value.zone_y }
  window.addEventListener('pointermove', onMove)
  window.addEventListener('pointerup', onUp)
}
function onEdgeDown(edge, e) {
  if (!selectedLaser.value || mode.value !== 'zone') return
  drag = { mode: edge, startPx: e.clientX, startPy: e.clientY, startScaleX: selectedLaser.value.zone_scale_x, startScaleY: selectedLaser.value.zone_scale_y }
  window.addEventListener('pointermove', onMove)
  window.addEventListener('pointerup', onUp)
}
function onMove(e) {
  if (!drag) return
  if (drag.mode === 'move') {
    const dx = (e.clientX - drag.startPx) / HALF
    const dy = (e.clientY - drag.startPy) / HALF
    push({ zone_x: clamp(drag.startX + dx, -1, 1), zone_y: clamp(drag.startY - dy, -1, 1) })
  } else if (drag.mode === 'right') {
    const d = (e.clientX - drag.startPx) / HALF
    push({ zone_scale_x: clamp(drag.startScaleX + d, 0.1, 2) })
  } else if (drag.mode === 'left') {
    const d = (e.clientX - drag.startPx) / HALF
    push({ zone_scale_x: clamp(drag.startScaleX - d, 0.1, 2) })
  } else if (drag.mode === 'bottom') {
    const d = (e.clientY - drag.startPy) / HALF
    push({ zone_scale_y: clamp(drag.startScaleY + d, 0.1, 2) })
  } else if (drag.mode === 'top') {
    const d = (e.clientY - drag.startPy) / HALF
    push({ zone_scale_y: clamp(drag.startScaleY - d, 0.1, 2) })
  }
}
function onUp(e) {
  if (!drag) return
  onMove(e)
  // Flush straight to the backend (not just the debounced call still
  // pending) so every drag reliably ends with a confirmed REST round-trip.
  const l = selectedLaser.value
  if (drag.mode === 'move') push({ zone_x: l.zone_x, zone_y: l.zone_y }, true)
  else if (drag.mode === 'left' || drag.mode === 'right') push({ zone_scale_x: l.zone_scale_x }, true)
  else push({ zone_scale_y: l.zone_scale_y }, true)
  drag = null
  window.removeEventListener('pointermove', onMove)
  window.removeEventListener('pointerup', onUp)
}

async function reset() {
  if (selectedId.value == null) return
  await updateLaser(selectedId.value, { zone_x: 0, zone_y: 0, zone_scale_x: 1, zone_scale_y: 1 }).catch(console.error)
}
</script>

<style scoped>
#zoning-panel {
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
#zoning-panel.popout {
  position:static; width:100%; height:100%; max-height:none;
  border:none; border-radius:0; box-sizing:border-box;
  padding:20px 22px 40px;
}
#zoning-panel.popout h2 { font-size:12px; }
#zoning-panel.popout button { padding:12px 10px; font-size:14px; }
#zoning-panel.popout .readout { font-size:13px; }
h2 { font-size:10px; letter-spacing:1.5px; text-transform:uppercase; margin:8px 0 4px; color:#9aa0bd; }
.laser-tag { color:#48e07a; text-transform:none; letter-spacing:0; }
.hint { font-size:10px; color:#6a7090; margin:0 0 8px; line-height:1.4; }
.hint.warn { color:#e08a8a; margin-top:8px; }
.empty { font-size:11px; color:#6a7090; }

.row { margin-bottom:8px; }
.laser-select {
  width:100%; box-sizing:border-box; background:rgba(255,255,255,0.06);
  border:1px solid rgba(255,255,255,0.15); color:#cfd3e6; border-radius:4px;
  padding:6px 8px; font-size:12px;
}
#zoning-panel.popout .laser-select { padding:10px; font-size:14px; }
.laser-status-row { display:flex; align-items:center; gap:6px; margin:-2px 0 8px; min-height:20px; }
.status-dot { width:7px; height:7px; border-radius:50%; background:#703030; flex:0 0 7px; }
.status-dot.on { background:#48e07a; }
.connection-label { flex:1; color:#6a7090; font-size:10px; }
.arm-status {
  border:1px solid rgba(255,255,255,0.18); border-radius:4px;
  background:rgba(255,255,255,0.07); color:#cfd3e6;
  padding:4px 8px; font-size:11px; white-space:nowrap;
}
.arm-status.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }

.mode-switch { display:flex; gap:0; margin:4px 0 6px; }
.mode-switch button { flex:1; border-radius:0; }
.mode-switch button:first-child { border-radius:4px 0 0 4px; }
.mode-switch button:last-child { border-radius:0 4px 4px 0; border-left:none; }
.mode-switch button.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }
.mode-switch button.masks.active { background:rgba(255,70,70,0.18); border-color:#ff4646; color:#ff7070; }
.count {
  display:inline-block; margin-left:6px; min-width:16px; padding:0 4px; border-radius:8px;
  background:rgba(255,70,70,0.35); color:#fff; font-size:10px; line-height:16px;
}

.stage {
  position:relative; margin:6px auto 10px; overflow:visible;
  border:1px dashed rgba(255,255,255,0.25); border-radius:4px;
  background:rgba(255,255,255,0.03);
}
.zone-box {
  position:absolute; box-sizing:border-box; cursor:move; touch-action:none;
  border:2px solid #48e07a; border-radius:2px; background:rgba(72,224,122,0.14);
}
.handle {
  position:absolute; background:#48e07a; border-radius:3px; touch-action:none;
}
.handle-left, .handle-right { width:10px; height:22px; top:50%; transform:translateY(-50%); cursor:ew-resize; }
.handle-left { left:-6px; }
.handle-right { right:-6px; }
.handle-top, .handle-bottom { width:22px; height:10px; left:50%; transform:translateX(-50%); cursor:ns-resize; }
.handle-top { top:-6px; }
.handle-bottom { bottom:-6px; }

/* Only the active layer is interactive; the other one stays visible but
   faded, so zone and masks can overlap without fighting over the pointer. */
.stage.mode-masks .zone-box { pointer-events:none; opacity:0.35; }
.stage.mode-masks .zone-box .handle { display:none; }
.stage.mode-zone .mask-box { pointer-events:none; opacity:0.5; }

.mask-box {
  position:absolute; box-sizing:border-box; cursor:move; touch-action:none;
  border:2px solid #ff4646; border-radius:1px;
  background:rgba(255,70,70,var(--fill, 0.3));
}
.mask-box.selected { border-color:#ff9a9a; box-shadow:0 0 0 1px rgba(255,154,154,0.5); z-index:2; }
.mask-box.lit { box-shadow:0 0 8px 1px rgba(255,40,40,0.9); }
.mask-label {
  position:absolute; left:50%; top:50%; transform:translate(-50%,-50%); font-size:9px; line-height:1; color:#ffd0d0;
  pointer-events:none; white-space:nowrap; text-shadow:0 0 2px #000;
}
.mhandle { position:absolute; width:12px; height:12px; background:#ff4646; border:1px solid #fff; border-radius:2px; touch-action:none; box-sizing:border-box; }
#zoning-panel.popout .mhandle { width:16px; height:16px; }
.mh-l  { left:-7px;  top:50%; transform:translateY(-50%); cursor:ew-resize; }
.mh-r  { right:-7px; top:50%; transform:translateY(-50%); cursor:ew-resize; }
.mh-t  { top:-7px;    left:50%; transform:translateX(-50%); cursor:ns-resize; }
.mh-b  { bottom:-7px; left:50%; transform:translateX(-50%); cursor:ns-resize; }
.mh-tl { left:-7px;  top:-7px;    cursor:nwse-resize; }
.mh-br { right:-7px; bottom:-7px; cursor:nwse-resize; }
.mh-tr { right:-7px; top:-7px;    cursor:nesw-resize; }
.mh-bl { left:-7px;  bottom:-7px; cursor:nesw-resize; }

.readout { display:flex; justify-content:space-between; font-size:11px; color:#9aa0bd; margin-bottom:4px; }
.readout b { color:#cfd3e6; font-weight:600; }

button { background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18); color:#cfd3e6; border-radius:4px; padding:4px 8px; font-size:11px; cursor:pointer; }
button:hover { background:rgba(255,255,255,0.14); }
button:disabled { opacity:0.4; cursor:default; }
.reset-btn { width:100%; margin-top:6px; padding:5px; font-size:11px; }

.mask-toolbar { display:flex; gap:6px; margin-bottom:6px; }
.add-btn { flex:1; border-color:#ff4646; color:#ff8080; }
.outline-off-btn { flex:1; }
.mask-row {
  border:1px solid rgba(255,255,255,0.12); border-radius:6px; padding:6px 8px; margin-bottom:6px;
  background:rgba(255,255,255,0.03);
}
.mask-row.selected { border-color:#ff4646; background:rgba(255,70,70,0.08); }
.mask-row-head { display:flex; align-items:center; gap:6px; }
.mask-name { flex:1; font-size:11px; color:#ffb0b0; font-weight:600; }
.outline-btn { white-space:nowrap; }
.outline-btn.active { background:rgba(255,40,40,0.25); border-color:#ff4646; color:#ffb0b0; }
.del-btn { padding:4px 7px; }
.reduce-row { display:flex; align-items:center; gap:8px; margin-top:6px; font-size:11px; color:#9aa0bd; }
.reduce-row input { flex:1; accent-color:#ff4646; }
.reduce-row b { min-width:36px; text-align:right; color:#cfd3e6; }
</style>
