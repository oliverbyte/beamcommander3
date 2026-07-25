<template>
  <div
    class="floating-panel"
    :class="{ maximized }"
    :style="{
      left: x + 'px', top: y + 'px',
      width: curWidth + 'px',
      height: curHeight != null ? curHeight + 'px' : undefined,
    }"
  >
    <div class="fp-header" @pointerdown="onDragStart">
      <span class="fp-title">{{ title }}</span>
      <div class="fp-actions">
        <button
          class="fp-btn"
          title="Open in its own window (so it can be dragged onto another screen)"
          @click="$emit('popout')"
        >⤢</button>
        <button class="fp-btn" title="Close" @click="$emit('close')">✕</button>
      </div>
    </div>
    <div class="fp-body">
      <slot />
    </div>

    <!-- Resize handles - mouse or touch, all 4 edges + 4 corners. Only
         offered on a maximized panel (currently just Cues): its grid
         reflows to fill whatever size results (see CuePanel.vue's fr-based
         rows/columns), so all 128 cue buttons stay visible and clickable
         at any panel size. -->
    <template v-if="maximized">
      <div class="rh rh-n"  @pointerdown="onResizeStart($event, 'n')"></div>
      <div class="rh rh-s"  @pointerdown="onResizeStart($event, 's')"></div>
      <div class="rh rh-e"  @pointerdown="onResizeStart($event, 'e')"></div>
      <div class="rh rh-w"  @pointerdown="onResizeStart($event, 'w')"></div>
      <div class="rh rh-ne" @pointerdown="onResizeStart($event, 'ne')"></div>
      <div class="rh rh-nw" @pointerdown="onResizeStart($event, 'nw')"></div>
      <div class="rh rh-se" @pointerdown="onResizeStart($event, 'se')"></div>
      <div class="rh rh-sw" @pointerdown="onResizeStart($event, 'sw')"></div>
    </template>
  </div>
</template>

<script setup>
// Draggable in-page panel, opened from TouchDock's big touch buttons. Kept
// as a plain div (not window.open) so tapping the dock button itself can
// never be flagged as an unsolicited popup - only the explicit "⤢" icon in
// the header actually opens a real, separate browser window (a direct
// click handler, same as before, so it still isn't blocked).
import { ref, onMounted, onBeforeUnmount } from 'vue'

const props = defineProps({
  title: { type: String, required: true },
  width: { type: Number, default: 300 },
  initialX: { type: Number, default: 60 },
  initialY: { type: Number, default: 60 },
  // When true, the panel always fills the available viewport (minus a
  // small margin) instead of using width/initialX/initialY - used by the
  // Cues panel so all 128 buttons stay visible with no scrolling. Tracks
  // window resizes live while open, unless the user has since manually
  // resized it themselves (see userResized below).
  maximized: { type: Boolean, default: false },
})
defineEmits(['popout', 'close'])

const MAX_MARGIN = 16
const MIN_W = 480
const MIN_H = 320

const curWidth = ref(props.maximized ? window.innerWidth - MAX_MARGIN * 2 : props.width)
const curHeight = ref(props.maximized ? window.innerHeight - MAX_MARGIN * 2 : null)

const x = ref(props.maximized ? MAX_MARGIN : props.initialX)
const y = ref(props.maximized ? MAX_MARGIN : props.initialY)

// Once the user drags a resize handle themselves, their chosen size wins -
// stop silently snapping back to fill the window on every resize event.
let userResized = false

function fitToWindow() {
  if (!props.maximized || userResized) return
  curWidth.value = window.innerWidth - MAX_MARGIN * 2
  curHeight.value = window.innerHeight - MAX_MARGIN * 2
  x.value = MAX_MARGIN
  y.value = MAX_MARGIN
}

onMounted(() => {
  if (props.maximized) window.addEventListener('resize', fitToWindow)
})
onBeforeUnmount(() => {
  window.removeEventListener('resize', fitToWindow)
  window.removeEventListener('pointermove', onResizeMove)
  window.removeEventListener('pointerup', onResizeEnd)
})

let dragging = false
let startPX = 0, startPY = 0, origX = 0, origY = 0

function onDragStart(e) {
  // Let clicks on the header's own icon buttons through untouched instead
  // of having the drag handler swallow them. A maximized panel already
  // fills the screen, so there's nowhere meaningful to drag it to - only
  // its resize handles (below) move it.
  if (e.target.closest('.fp-btn') || props.maximized) return
  dragging = true
  startPX = e.clientX
  startPY = e.clientY
  origX = x.value
  origY = y.value
  window.addEventListener('pointermove', onDragMove)
  window.addEventListener('pointerup', onDragEnd)
}
function onDragMove(e) {
  if (!dragging) return
  const margin = 40
  x.value = Math.min(Math.max(origX + (e.clientX - startPX), -props.width + margin), window.innerWidth - margin)
  y.value = Math.min(Math.max(origY + (e.clientY - startPY), 0), window.innerHeight - margin)
}
function onDragEnd() {
  dragging = false
  window.removeEventListener('pointermove', onDragMove)
  window.removeEventListener('pointerup', onDragEnd)
}

// Resizing (maximized panel only) - one handler for all 8 edge/corner
// handles, driven by which of n/s/e/w letters are in `dir`.
let resizeDir = null
let rStartPX = 0, rStartPY = 0, rStartW = 0, rStartH = 0, rStartX = 0, rStartY = 0

function onResizeStart(e, dir) {
  e.stopPropagation()
  e.preventDefault()
  userResized = true
  resizeDir = dir
  rStartPX = e.clientX
  rStartPY = e.clientY
  rStartW = curWidth.value
  rStartH = curHeight.value
  rStartX = x.value
  rStartY = y.value
  window.addEventListener('pointermove', onResizeMove)
  window.addEventListener('pointerup', onResizeEnd)
}
function onResizeMove(e) {
  if (!resizeDir) return
  const dx = e.clientX - rStartPX
  const dy = e.clientY - rStartPY
  if (resizeDir.includes('e')) {
    curWidth.value = Math.max(MIN_W, Math.min(rStartW + dx, window.innerWidth - rStartX - 4))
  }
  if (resizeDir.includes('s')) {
    curHeight.value = Math.max(MIN_H, Math.min(rStartH + dy, window.innerHeight - rStartY - 4))
  }
  if (resizeDir.includes('w')) {
    const newW = Math.max(MIN_W, Math.min(rStartW - dx, rStartX + rStartW - 4))
    x.value = rStartX + (rStartW - newW)
    curWidth.value = newW
  }
  if (resizeDir.includes('n')) {
    const newH = Math.max(MIN_H, Math.min(rStartH - dy, rStartY + rStartH - 4))
    y.value = rStartY + (rStartH - newH)
    curHeight.value = newH
  }
}
function onResizeEnd() {
  resizeDir = null
  window.removeEventListener('pointermove', onResizeMove)
  window.removeEventListener('pointerup', onResizeEnd)
}
</script>

<style scoped>
.floating-panel {
  position:fixed; z-index:40; max-height:80vh; display:flex; flex-direction:column;
  background:rgba(8,10,18,0.94); border:1px solid rgba(120,130,200,0.3);
  border-radius:10px; box-shadow:0 12px 40px rgba(0,0,0,0.55);
  backdrop-filter:blur(6px); color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
}
.floating-panel.maximized { max-height:none; }
.fp-header {
  display:flex; align-items:center; justify-content:space-between; gap:8px;
  padding:10px 10px 10px 14px; cursor:move; touch-action:none; user-select:none;
  border-bottom:1px solid rgba(255,255,255,0.1); flex-shrink:0;
}
.floating-panel.maximized .fp-header { cursor:default; }
.fp-title { font-size:13px; letter-spacing:1.5px; text-transform:uppercase; color:#8fe3ff; }
.fp-actions { display:flex; gap:6px; }
.fp-btn {
  width:34px; height:34px; display:flex; align-items:center; justify-content:center;
  background:rgba(255,255,255,0.08); border:1px solid rgba(255,255,255,0.2);
  color:#cfd3e6; border-radius:6px; font-size:15px; cursor:pointer; padding:0;
  touch-action:manipulation;
}
.fp-btn:hover { background:rgba(255,255,255,0.18); }
.fp-body { overflow-y:auto; flex:1; min-height:0; }

/* Resize handles: thin strips along each edge, small squares at each
   corner (corners take priority visually by sitting on top, z-index
   above the edge strips). touch-action:none so a finger drag resizes
   instead of scrolling the page underneath. */
.rh { position:absolute; touch-action:none; z-index:2; }
.rh-n, .rh-s { left:12px; right:12px; height:10px; cursor:ns-resize; }
.rh-e, .rh-w { top:12px; bottom:12px; width:10px; cursor:ew-resize; }
.rh-n { top:-5px; }
.rh-s { bottom:-5px; }
.rh-w { left:-5px; }
.rh-e { right:-5px; }
.rh-ne, .rh-nw, .rh-se, .rh-sw { width:18px; height:18px; z-index:3; }
.rh-ne { top:-5px; right:-5px; cursor:nesw-resize; }
.rh-nw { top:-5px; left:-5px; cursor:nwse-resize; }
.rh-se { bottom:-5px; right:-5px; cursor:nwse-resize; }
.rh-sw { bottom:-5px; left:-5px; cursor:nesw-resize; }
</style>
