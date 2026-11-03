<template>
  <div id="cue-panel" :class="{ popout }">
    <div class="panel-body">
      <div class="mode-row">
        <button class="mode-btn save-mode-btn" :class="{ active: saveArmed }" @click="toggleSaveMode">
          {{ saveArmed ? '● tap slot' : '◌ Save' }}
        </button>
        <button class="mode-btn move-mode-btn" :class="{ active: moveArmed }" @click="toggleMoveMode">
          {{ moveSource ? `⇄ to…` : (moveArmed ? '⇄ pick slot' : '⇄ Move') }}
        </button>
      </div>
      <div class="cue-grid">
        <button
          v-for="n in CUE_COUNT" :key="n"
          class="cue-btn"
          :class="{ populated: !!cues[n], 'current-cue': isCurrentCue(n), armed: saveArmed, 'move-armed': moveArmed, 'move-source': moveSource === n }"
          @click="onClick(n)"
          @contextmenu.prevent="onClear(n)"
          :title="cueTitle(n)"
        >
          <CuePreview v-if="cues[n]" :cue="cues[n]" />
          <span class="cue-num" :class="{ empty: !cues[n] }">{{ n }}</span>
        </button>
      </div>
      <p class="hint">Right-click a slot to clear it.</p>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import CuePreview from './CuePreview.vue'
import { cues, isCueCurrent as isCurrentCue, saveCue, recallCue, clearCue, moveCue, fetchCues } from '../composables/useLaserSocket.js'

const { popout } = defineProps({ popout: { type: Boolean, default: false } })
const CUE_COUNT = 128
const saveArmed = ref(false)
const moveArmed = ref(false)
const moveSource = ref(null)

onMounted(() => { fetchCues().catch(() => {}) })

function toggleSaveMode() {
  saveArmed.value = !saveArmed.value
  moveArmed.value = false
  moveSource.value = null
}

function toggleMoveMode() {
  moveArmed.value = !moveArmed.value
  moveSource.value = null
  saveArmed.value = false
}

function cueTitle(n) {
  if (moveArmed.value) {
    return moveSource.value
      ? (moveSource.value === n ? `Cue ${n} — click again to cancel` : `Move cue ${moveSource.value} into slot ${n}${cues[n] ? ' (overwrites it)' : ''}`)
      : (cues[n] ? `Cue ${n} — click to pick as the one to move` : `Cue ${n} — empty, nothing to move`)
  }
  return cues[n] ? `Cue ${n} — click to recall, right-click to clear` : `Cue ${n} — empty, arm save mode then click to save`
}

async function onClick(n) {
  if (moveArmed.value) {
    if (!moveSource.value) {
      if (cues[n]) moveSource.value = n
    } else if (moveSource.value === n) {
      moveSource.value = null // clicking the source again cancels
    } else {
      await moveCue(moveSource.value, n).catch(console.error)
      moveSource.value = null
      moveArmed.value = false
    }
    return
  }
  if (saveArmed.value) {
    await saveCue(n).catch(console.error)
    saveArmed.value = false
  } else if (cues[n]) {
    await recallCue(n).catch(console.error)
  }
}

async function onClear(n) {
  if (!cues[n]) return
  await clearCue(n).catch(console.error)
}
</script>

<style scoped>
#cue-panel {
  position:fixed; top:12px; right:12px; width:1360px;
  background:rgba(8,10,18,0.88); border:1px solid rgba(120,130,200,0.25);
  border-radius:10px; padding:14px 16px; backdrop-filter:blur(6px);
  user-select:none; z-index:10; color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  max-height:calc(100vh - 24px); overflow-y:auto;
}
/* Popped out - either into its own draggable browser window, or embedded
   in a FloatingPanel div (see TouchDock.vue). height:100% (not 100vh) so
   it fills whichever parent it's actually in. */
#cue-panel.popout {
  position:static; width:100%; height:100%; max-height:none;
  border:none; border-radius:0; box-sizing:border-box;
  padding:20px 22px 40px;
}
#cue-panel.popout .panel-body { display:flex; flex-direction:column; height:100%; }
#cue-panel.popout .mode-row { flex:0 0 auto; }
#cue-panel.popout .mode-btn { padding:14px 6px; font-size:13px; }
/* Grid fills all remaining height after the mode row/hint - 8 fr-sized
   rows (not a fixed/square button size) so all 128 slots always fit
   exactly, with no internal scrolling, at whatever size the maximized
   window ends up being. */
#cue-panel.popout .cue-grid {
  flex:1 1 auto; min-height:0; gap:10px;
  grid-template-rows:repeat(8,1fr);
}
#cue-panel.popout .cue-btn { aspect-ratio:auto; }
#cue-panel.popout .cue-num.empty { font-size:22px; }
#cue-panel.popout .hint { flex:0 0 auto; font-size:12px; }
.mode-row { display:flex; gap:6px; margin-bottom:10px; }
.mode-btn {
  flex:1; min-width:0; padding:6px 4px; font-size:11px; letter-spacing:0.3px;
  background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18);
  color:#cfd3e6; border-radius:4px; cursor:pointer; white-space:nowrap;
  overflow:hidden; text-overflow:ellipsis;
}
.mode-btn:hover { background:rgba(255,255,255,0.14); }
.save-mode-btn.active { background:rgba(255,60,60,0.25); border-color:#ff4040; color:#ff8080; }
.move-mode-btn.active { background:rgba(143,227,255,0.2); border-color:#8fe3ff; color:#8fe3ff; }
/* 16 across x 8 down = 128 cue slots, each double the original size. */
.cue-grid { display:grid; grid-template-columns:repeat(16,1fr); gap:8px; }
.cue-btn {
  position:relative; aspect-ratio:1; background:rgba(255,255,255,0.05);
  border:1px solid rgba(255,255,255,0.15);
  color:#6a7090; border-radius:6px; font-size:13px; cursor:pointer;
  padding:0; overflow:hidden;
}
.cue-btn:hover { background:rgba(255,255,255,0.12); }
.cue-btn.populated { background:rgba(20,22,32,0.9); border-color:#48e07a; color:#48e07a; }
.cue-btn.current-cue {
  border:3px solid #a8ff60;
  box-shadow:inset 0 0 0 1px rgba(168,255,96,0.65),0 0 12px rgba(168,255,96,0.75);
  color:#c2ff89;
}
.cue-btn.armed { border-color:#ff4040; }
.cue-btn.armed:hover { background:rgba(255,60,60,0.2); }
.cue-btn.move-armed { border-color:rgba(143,227,255,0.5); }
.cue-btn.move-armed:hover { background:rgba(143,227,255,0.2); }
.cue-btn.move-source { background:rgba(143,227,255,0.3); border-color:#8fe3ff; color:#8fe3ff; }
.cue-num {
  position:absolute; bottom:2px; right:5px; font-size:13px; line-height:1;
  color:rgba(207,211,230,0.75); text-shadow:0 0 2px rgba(0,0,0,0.9);
  pointer-events:none;
}
.cue-num.empty {
  inset:0; bottom:auto; right:auto;
  display:flex; align-items:center; justify-content:center;
  width:100%; height:100%; font-size:18px; color:inherit; text-shadow:none;
}
.hint { font-size:10px; color:#6a7090; margin:8px 0 0; line-height:1.4; }
</style>
