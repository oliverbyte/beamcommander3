<template>
  <div class="floating-panel" :style="{ left: x + 'px', top: y + 'px', width: width + 'px' }">
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
  </div>
</template>

<script setup>
// Draggable in-page panel, opened from TouchDock's big touch buttons. Kept
// as a plain div (not window.open) so tapping the dock button itself can
// never be flagged as an unsolicited popup - only the explicit "⤢" icon in
// the header actually opens a real, separate browser window (a direct
// click handler, same as before, so it still isn't blocked).
import { ref } from 'vue'

const props = defineProps({
  title: { type: String, required: true },
  width: { type: Number, default: 300 },
  initialX: { type: Number, default: 60 },
  initialY: { type: Number, default: 60 },
})
defineEmits(['popout', 'close'])

const x = ref(props.initialX)
const y = ref(props.initialY)

let dragging = false
let startPX = 0, startPY = 0, origX = 0, origY = 0

function onDragStart(e) {
  // Let clicks on the header's own icon buttons through untouched instead
  // of having the drag handler swallow them.
  if (e.target.closest('.fp-btn')) return
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
</script>

<style scoped>
.floating-panel {
  position:fixed; z-index:40; max-height:80vh; display:flex; flex-direction:column;
  background:rgba(8,10,18,0.94); border:1px solid rgba(120,130,200,0.3);
  border-radius:10px; box-shadow:0 12px 40px rgba(0,0,0,0.55);
  backdrop-filter:blur(6px); color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
}
.fp-header {
  display:flex; align-items:center; justify-content:space-between; gap:8px;
  padding:10px 10px 10px 14px; cursor:move; touch-action:none; user-select:none;
  border-bottom:1px solid rgba(255,255,255,0.1); flex-shrink:0;
}
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
</style>
