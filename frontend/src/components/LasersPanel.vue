<template>
  <div id="lasers-panel" :class="{ popout }">
    <div class="panel-body">
      <p class="hint">Add each physical laser (EtherDream DAC) by its IP
        address, then arm it to start streaming the show to it. Any number
        of lasers can be armed at once - they all output the exact same
        show in parallel (Zone 1), each transformed by its own calibration
        set in the Zoning panel. Drag the grip to change installation order;
        movement phase offsets follow that order.</p>

      <div class="laser-list">
        <div
          v-for="l in lasers"
          :key="l.id"
          class="laser-row"
          :class="{ 'drop-target': dropTargetId === l.id }"
          @dragover.prevent="onDragOver($event, l.id)"
          @drop.prevent="onDrop($event, l.id)"
        >
          <span
            class="drag-handle"
            draggable="true"
            :title="`Drag to reorder ${l.name}`"
            :aria-label="`Drag to reorder ${l.name}`"
            @dragstart.stop="onDragStart($event, l.id)"
            @dragend="onDragEnd"
          >⠿</span>
          <span class="dot" :class="{ on: l.connected }" :title="l.connected ? 'connected' : 'not connected'"></span>
          <input
            class="name"
            :value="l.name"
            @change="onRename(l, $event.target.value)"
          />
          <input
            class="ip"
            :value="l.ip"
            placeholder="IP address"
            @change="onReip(l, $event.target.value)"
          />
          <button
            class="arm-btn"
            :class="{ active: l.armed }"
            :title="l.armed ? 'Disarm (stop streaming)' : 'Arm (start streaming Zone 1)'"
            @click="toggleArmed(l)"
          >{{ l.armed ? '● Armed' : '○ Armed' }}</button>
          <button
            class="mirror-btn"
            :class="{ active: l.mirror_x }"
            :title="l.mirror_x ? 'Un-mirror X axis (hardware only)' : 'Mirror X axis (hardware only)'"
            @click="toggleMirror(l)"
          >⇆</button>
          <button
            class="mirror-btn"
            :class="{ active: l.mirror_y }"
            :title="l.mirror_y ? 'Un-mirror Y axis (hardware only)' : 'Mirror Y axis (hardware only)'"
            @click="toggleMirrorY(l)"
          >⇅</button>
          <button class="del-btn" title="Remove laser" @click="onDelete(l)">✕</button>
        </div>
        <p v-if="!lasers.length" class="empty">No lasers configured yet.</p>
      </div>

      <hr />
      <h2>Add laser</h2>
      <div class="row">
        <input v-model="newName" placeholder="Name (e.g. Laser 2)" />
      </div>
      <div class="row">
        <input v-model="newIp" placeholder="IP address" />
      </div>
      <button class="add-btn" :disabled="!newIp" @click="onAdd">+ Add laser</button>
      <p class="error" v-if="error">{{ error }}</p>

      <hr />
      <h2>Test pattern</h2>
      <p class="hint">Official ILDA Standard Test Pattern (ILDA's own files)
        for scanner tuning. Every armed laser shows it instead of the show, at
        exactly the point rate the standard requires: the scan rate (Beam) is
        set to 12 or 30 kpps while it's on and restored when it's switched
        off. Scan it small while tuning (30K: about 8° optical or less).
        Blackout, master brightness, zone and safety masks still apply.</p>
      <div class="tp-buttons">
        <button
          class="tp-btn"
          :class="{ active: laserState.test_pattern === 'off' }"
          @click="selectTestPattern('off')"
        >Off</button>
        <button
          v-for="t in testPatterns"
          :key="t.id"
          class="tp-btn"
          :class="{ active: laserState.test_pattern === t.id }"
          :disabled="t.kpps > laserState.max_rate_kpps"
          :title="t.kpps > laserState.max_rate_kpps
            ? `Needs ${t.kpps} kpps - above the max scan rate (${laserState.max_rate_kpps} kpps)`
            : `Show ${t.label} on all armed lasers at ${t.kpps} kpps`"
          @click="selectTestPattern(t.id)"
        >{{ t.label }}</button>
      </div>
      <label class="tp-size">
        <span>Size</span>
        <input type="range" min="5" max="100" step="1" :value="sizePercent" @input="onSize($event.target.value)" />
        <span class="tp-size-value">{{ sizePercent }}%</span>
      </label>
      <p v-if="testPatternStatus" class="tp-status" :class="{ warn: testPatternWarn }">{{ testPatternStatus }}</p>
    </div>
  </div>
</template>

<script setup>
// Lists every configured laser (physical EtherDream DAC) and lets the
// operator add/rename/re-IP/remove one, and arm/disarm it (see `armed`'s
// comment in laser_daemon.cpp). All actions go straight through the REST
// API (GET/POST/DELETE /api/lasers) - the backend (laser_thread()) is the
// only thing that actually opens/manages the DAC connections; this panel
// just edits the configured list and its armed flag. Any number of lasers
// can be armed simultaneously, in which case laser_thread() streams the
// same Zone 1 show to all of them in parallel (not sequentially), so
// arming more lasers never adds lag to the ones already streaming. Each
// laser's own pan/zoom calibration is set separately, in the Zoning panel.
import { ref, computed, onMounted } from 'vue'
import {
  lasers, fetchLasers, addLaser, updateLaser, deleteLaser, reorderLasers,
  laserState, testPatterns, fetchTestPatterns, setTestPattern,
} from '../composables/useLaserSocket.js'

const { popout } = defineProps({ popout: { type: Boolean, default: false } })

const newName = ref('')
const newIp = ref('')
const error = ref(null)
const draggedId = ref(null)
const dropTargetId = ref(null)

onMounted(() => {
  fetchLasers().catch(() => {})
  fetchTestPatterns().catch(() => {})
})

// ── Test pattern (see backend/test_patterns.h) ────────────────────────────
const sizePercent = computed(() => Math.round((laserState.test_pattern_size ?? 1) * 100))
const activePattern = computed(() => testPatterns.find((t) => t.id === laserState.test_pattern))
const armedCount = computed(() => lasers.filter((l) => l.armed).length)
// ILDA: the pattern only works within +-1% of its rate - the daemon keeps lasers dark otherwise.
const rateMismatch = computed(() => {
  const t = activePattern.value
  return !!t && Math.abs(laserState.rate_kpps - t.kpps) > t.kpps * 0.01
})
const testPatternWarn = computed(() => !!activePattern.value && (laserState.blackout || armedCount.value === 0 || rateMismatch.value))
const testPatternStatus = computed(() => {
  const t = activePattern.value
  if (!t) return ''
  if (armedCount.value === 0) return `${t.label} is active, but no laser is armed.`
  if (rateMismatch.value) return `${t.label} needs ${t.kpps} kpps, but the scan rate is ${laserState.rate_kpps} kpps - lasers stay dark. Select ${t.label} again to reset the rate.`
  if (laserState.blackout) return `${t.label} is active - blackout is on, nothing is output.`
  return `${t.label} on ${armedCount.value} armed laser${armedCount.value === 1 ? '' : 's'} at ${t.kpps} kpps.`
})

async function selectTestPattern(id) {
  try { await setTestPattern({ pattern: id }); error.value = null } catch (e) { error.value = String(e.message || e) }
}

// The slider updates locally right away; the daemon gets at most one request per 60 ms.
let sizeTimer = null
function onSize(percent) {
  laserState.test_pattern_size = Number(percent) / 100
  clearTimeout(sizeTimer)
  sizeTimer = setTimeout(async () => {
    try { await setTestPattern({ size: laserState.test_pattern_size }); error.value = null }
    catch (e) { error.value = String(e.message || e) }
  }, 60)
}

async function onAdd() {
  if (!newIp.value) return
  try {
    await addLaser(newName.value, newIp.value)
    newName.value = ''
    newIp.value = ''
    error.value = null
  } catch (e) { error.value = String(e) }
}

async function onRename(l, name) {
  try { await updateLaser(l.id, { name }); error.value = null } catch (e) { error.value = String(e) }
}
async function onReip(l, ip) {
  try { await updateLaser(l.id, { ip }); error.value = null } catch (e) { error.value = String(e) }
}
async function toggleArmed(l) {
  try { await updateLaser(l.id, { armed: !l.armed }); error.value = null }
  catch (e) { error.value = String(e) }
}
async function toggleMirror(l) {
  try { await updateLaser(l.id, { mirror_x: !l.mirror_x }); error.value = null }
  catch (e) { error.value = String(e) }
}
async function toggleMirrorY(l) {
  try { await updateLaser(l.id, { mirror_y: !l.mirror_y }); error.value = null }
  catch (e) { error.value = String(e) }
}
async function onDelete(l) {
  try { await deleteLaser(l.id); error.value = null } catch (e) { error.value = String(e) }
}

function onDragStart(event, id) {
  draggedId.value = id
  event.dataTransfer.effectAllowed = 'move'
  event.dataTransfer.setData('text/plain', String(id))
}
function onDragOver(event, id) {
  if (draggedId.value == null || draggedId.value === id) return
  event.dataTransfer.dropEffect = 'move'
  dropTargetId.value = id
}
async function onDrop(event, targetId) {
  const sourceId = Number(event.dataTransfer.getData('text/plain')) || draggedId.value
  const ids = lasers.map((laser) => laser.id)
  const sourceIndex = ids.indexOf(sourceId)
  const targetIndex = ids.indexOf(targetId)
  if (sourceIndex < 0 || targetIndex < 0 || sourceIndex === targetIndex) return
  ids.splice(sourceIndex, 1)
  ids.splice(sourceIndex < targetIndex ? targetIndex - 1 : targetIndex, 0, sourceId)
  try { await reorderLasers(ids); error.value = null }
  catch (e) { error.value = String(e) }
  onDragEnd()
}
function onDragEnd() {
  draggedId.value = null
  dropTargetId.value = null
}
</script>

<style scoped>
#lasers-panel {
  position:fixed; top:52px; left:12px; width:300px;
  background:rgba(8,10,18,0.88); border:1px solid rgba(120,130,200,0.25);
  border-radius:10px; padding:14px 16px; backdrop-filter:blur(6px);
  user-select:none; z-index:10; color:#cfd3e6;
  font-family:-apple-system,"Segoe UI",Roboto,sans-serif;
  max-height:calc(100vh - 64px); overflow-y:auto;
}
#lasers-panel.popout {
  position:static; width:100%; height:100%; max-height:none;
  border:none; border-radius:0; box-sizing:border-box;
  padding:20px 22px 40px;
}
h2 { font-size:10px; letter-spacing:1.5px; text-transform:uppercase; margin:8px 0 4px; color:#9aa0bd; }
.hint { font-size:10px; color:#6a7090; margin:0 0 8px; line-height:1.4; }

.laser-list { display:flex; flex-direction:column; gap:6px; margin-bottom:6px; }
.laser-row {
  display:flex; align-items:center; gap:5px;
  background:rgba(255,255,255,0.04); border:1px solid rgba(255,255,255,0.1);
  border-radius:6px; padding:5px 6px;
}
.laser-row.drop-target { border-color:#48e07a; background:rgba(72,224,122,0.1); }
.drag-handle { color:#8a90a8; cursor:grab; font-size:14px; line-height:1; user-select:none; }
.drag-handle:active { cursor:grabbing; }
.dot { width:7px; height:7px; border-radius:50%; background:#703030; flex-shrink:0; }
.dot.on { background:#48e07a; }
.empty { font-size:11px; color:#6a7090; }

input.name, input.ip {
  background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.15);
  color:#cfd3e6; border-radius:4px; padding:4px 6px; font-size:11px; min-width:0;
}
input.name { flex:1 1 40%; }
input.ip { flex:1 1 45%; }

button { background:rgba(255,255,255,0.07); border:1px solid rgba(255,255,255,0.18); color:#cfd3e6; border-radius:4px; padding:4px 8px; font-size:11px; cursor:pointer; }
button:hover { background:rgba(255,255,255,0.14); }
button:disabled { opacity:0.5; cursor:default; }
.arm-btn { flex-shrink:0; white-space:nowrap; }
.arm-btn.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }
.mirror-btn { flex-shrink:0; padding:4px 7px; }
.mirror-btn.active { background:rgba(120,170,255,0.18); border-color:#78aaff; color:#78aaff; }
.del-btn { flex-shrink:0; padding:4px 7px; color:#ff8080; border-color:rgba(255,100,100,0.35); }

.tp-buttons { display:flex; gap:5px; margin-bottom:8px; }
.tp-btn { flex:1 1 0; padding:6px 4px; }
.tp-btn.active { background:rgba(255,200,60,0.18); border-color:#ffc83c; color:#ffc83c; }
.tp-size { display:flex; align-items:center; gap:8px; font-size:11px; color:#9aa0bd; margin-bottom:6px; }
.tp-size input { flex:1; min-width:0; }
.tp-size-value { width:34px; text-align:right; font-variant-numeric:tabular-nums; }
.tp-status { font-size:11px; color:#ffc83c; margin:2px 0 4px; }
.tp-status.warn { color:#ff8080; }

.row { margin-bottom:6px; }
.row input { width:100%; box-sizing:border-box; background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.15); color:#cfd3e6; border-radius:4px; padding:6px 8px; font-size:12px; }
.add-btn { width:100%; margin-top:2px; padding:6px; font-size:12px; }
hr { border:none; border-top:1px solid rgba(255,255,255,0.1); margin:8px 0; }
.error { font-size:11px; color:#ff8080; word-break:break-word; margin-top:4px; }
</style>
