<template>
  <div id="cue-panel" :class="{ collapsed }">
    <button class="collapse-btn" @click="collapsed = !collapsed" :title="collapsed ? 'Show cues' : 'Hide cues'">
      {{ collapsed ? '☰' : '✕' }}
    </button>
    <div class="panel-body" v-show="!collapsed">
      <h1>Cues</h1>
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
          :class="{ populated: !!cues[n], armed: saveArmed, 'move-armed': moveArmed, 'move-source': moveSource === n }"
          @click="onClick(n)"
          @contextmenu.prevent="onClear(n)"
          :title="cueTitle(n)"
        >{{ n }}</button>
      </div>
      <p class="hint">Right-click a slot to clear it.</p>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { cues, saveCue, recallCue, clearCue, moveCue, fetchCues } from '../composables/useLaserSocket.js'

const CUE_COUNT = 32
const collapsed = ref(false)
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
  position:fixed; top:12px; right:12px; width:220px;
  background:rgba(8,10,18,0.88); border:1px solid rgba(120,130,200,0.25);
  border-radius:10px; padding:14px 16px; backdrop-filter:blur(6px);
  user-select:none; z-index:10; color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  max-height:calc(100vh - 24px); overflow-y:auto;
}
#cue-panel.collapsed {
  width:auto; max-height:none; overflow:visible; padding:8px;
}
.collapse-btn {
  position:absolute; top:10px; right:10px; z-index:11;
  width:22px; height:22px; padding:0; line-height:1;
  font-size:12px; display:flex; align-items:center; justify-content:center;
  background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18);
  color:#cfd3e6; border-radius:4px; cursor:pointer;
}
.collapse-btn:hover { background:rgba(255,255,255,0.14); }
#cue-panel.collapsed .collapse-btn { position:static; }
.panel-body { padding-right:26px; }
h1 { font-size:13px; letter-spacing:2px; text-transform:uppercase; margin:0 0 8px; color:#8fe3ff; }
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
.cue-grid { display:grid; grid-template-columns:repeat(4,1fr); gap:6px; }
.cue-btn {
  aspect-ratio:1; background:rgba(255,255,255,0.05); border:1px solid rgba(255,255,255,0.15);
  color:#6a7090; border-radius:6px; font-size:13px; cursor:pointer;
}
.cue-btn:hover { background:rgba(255,255,255,0.12); }
.cue-btn.populated { background:rgba(72,224,122,0.16); border-color:#48e07a; color:#48e07a; }
.cue-btn.armed { border-color:#ff4040; }
.cue-btn.armed:hover { background:rgba(255,60,60,0.2); }
.cue-btn.move-armed { border-color:rgba(143,227,255,0.5); }
.cue-btn.move-armed:hover { background:rgba(143,227,255,0.2); }
.cue-btn.move-source { background:rgba(143,227,255,0.3); border-color:#8fe3ff; color:#8fe3ff; }
.hint { font-size:10px; color:#6a7090; margin:8px 0 0; line-height:1.4; }
</style>
