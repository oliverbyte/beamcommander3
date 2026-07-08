<template>
  <div id="zoning-panel" :class="{ popout }">
    <div class="panel-body">
      <p class="hint">Drag the box to move it, drag an edge to scale that
        axis only. Affects the real laser output only - the on-screen
        preview always shows the full, un-zoned show.</p>

      <h2>Zone 1 <span class="laser-tag">→ {{ selectedLaser ? selectedLaser.name : '(no lasers)' }}</span></h2>

      <div class="row" v-if="lasers.length">
        <select class="laser-select" v-model.number="selectedId">
          <option v-for="l in lasers" :key="l.id" :value="l.id">
            {{ l.name }}{{ l.armed ? ' ●' : '' }}
          </option>
        </select>
      </div>
      <p v-else class="empty">No lasers configured yet - add one in the Lasers panel first.</p>

      <template v-if="selectedLaser">
        <div class="stage" :style="{ width: CANVAS + 'px', height: CANVAS + 'px' }">
          <div class="zone-box" :style="boxStyle" @pointerdown="onBodyDown">
            <div class="handle handle-left"   @pointerdown.stop="onEdgeDown('left', $event)"></div>
            <div class="handle handle-right"  @pointerdown.stop="onEdgeDown('right', $event)"></div>
            <div class="handle handle-top"    @pointerdown.stop="onEdgeDown('top', $event)"></div>
            <div class="handle handle-bottom" @pointerdown.stop="onEdgeDown('bottom', $event)"></div>
          </div>
        </div>

        <div class="readout">
          <span>X <b>{{ fmt(selectedLaser.zone_x) }}</b></span>
          <span>Y <b>{{ fmt(selectedLaser.zone_y) }}</b></span>
          <span>W <b>{{ fmt(selectedLaser.zone_scale_x) }}</b></span>
          <span>H <b>{{ fmt(selectedLaser.zone_scale_y) }}</b></span>
        </div>

        <button class="reset-btn" @click="reset">↺ Reset zone</button>
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
import { computed, onMounted, ref, watch } from 'vue'
import { lasers, fetchLasers, updateLaser, patchLaserLocal } from '../composables/useLaserSocket.js'

const { popout } = defineProps({ popout: { type: Boolean, default: false } })

const selectedId = ref(null)

const selectedLaser = computed(() => lasers.find((l) => l.id === selectedId.value) || null)

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

onMounted(() => { fetchLasers().catch(() => {}) })

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

let debounce = null
function push(partial, flush = false) {
  const id = selectedId.value
  if (id == null) return
  patchLaserLocal(id, partial)
  clearTimeout(debounce)
  if (flush) updateLaser(id, partial).catch(console.error)
  else debounce = setTimeout(() => updateLaser(id, partial).catch(console.error), 80)
}

// pointerdown/move/up (not drag events) so this works identically with
// mouse and touch - window-level move/up listeners (not just on the
// handle/box itself) so the drag keeps tracking even if the pointer
// outruns the small element while moving fast.
let drag = null
function onBodyDown(e) {
  if (!selectedLaser.value) return
  drag = { mode: 'move', startPx: e.clientX, startPy: e.clientY, startX: selectedLaser.value.zone_x, startY: selectedLaser.value.zone_y }
  window.addEventListener('pointermove', onMove)
  window.addEventListener('pointerup', onUp)
}
function onEdgeDown(edge, e) {
  if (!selectedLaser.value) return
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
.empty { font-size:11px; color:#6a7090; }

.row { margin-bottom:8px; }
.laser-select {
  width:100%; box-sizing:border-box; background:rgba(255,255,255,0.06);
  border:1px solid rgba(255,255,255,0.15); color:#cfd3e6; border-radius:4px;
  padding:6px 8px; font-size:12px;
}
#zoning-panel.popout .laser-select { padding:10px; font-size:14px; }

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

.readout { display:flex; justify-content:space-between; font-size:11px; color:#9aa0bd; margin-bottom:4px; }
.readout b { color:#cfd3e6; font-weight:600; }

button { background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18); color:#cfd3e6; border-radius:4px; padding:4px 8px; font-size:11px; cursor:pointer; }
button:hover { background:rgba(255,255,255,0.14); }
.reset-btn { width:100%; margin-top:6px; padding:5px; font-size:11px; }
</style>
