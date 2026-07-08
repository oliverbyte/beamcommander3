<template>
  <div id="lasers-panel" :class="{ popout }">
    <div class="panel-body">
      <p class="hint">Add each physical laser (EtherDream DAC) by its IP
        address, then assign it to Zone 1 to start streaming the show to
        it. Any number of lasers can be assigned to Zone 1 at once - they
        all output the exact same (zone-calibrated) show in parallel.</p>

      <div class="laser-list">
        <div v-for="l in lasers" :key="l.id" class="laser-row">
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
            class="assign-btn"
            :class="{ active: l.assigned_zone === 1 }"
            @click="toggleAssign(l)"
          >{{ l.assigned_zone === 1 ? '● Zone 1' : '○ Zone 1' }}</button>
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
    </div>
  </div>
</template>

<script setup>
// Lists every configured laser (physical EtherDream DAC) and lets the
// operator add/rename/re-IP/remove one, and assign it to Zone 1 (the only
// zone that exists today - see zone_x/zone_y/zone_scale_x/zone_scale_y's
// comment in laser_daemon.cpp). All actions go straight through the REST
// API (GET/POST/DELETE /api/lasers) - the backend (laser_thread()) is the
// only thing that actually opens/manages the DAC connections; this panel
// just edits the configured list and its assignment. Any number of lasers
// can be assigned to Zone 1 simultaneously, in which case laser_thread()
// streams the same output to all of them in parallel (not sequentially),
// so adding more assigned lasers never adds lag to existing ones.
import { ref, onMounted } from 'vue'
import { lasers, fetchLasers, addLaser, updateLaser, deleteLaser } from '../composables/useLaserSocket.js'

const { popout } = defineProps({ popout: { type: Boolean, default: false } })

const newName = ref('')
const newIp = ref('')
const error = ref(null)

onMounted(() => { fetchLasers().catch(() => {}) })

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
async function toggleAssign(l) {
  try { await updateLaser(l.id, { assigned_zone: l.assigned_zone === 1 ? 0 : 1 }); error.value = null }
  catch (e) { error.value = String(e) }
}
async function onDelete(l) {
  try { await deleteLaser(l.id); error.value = null } catch (e) { error.value = String(e) }
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
.assign-btn { flex-shrink:0; white-space:nowrap; }
.assign-btn.active { background:rgba(72,224,122,0.18); border-color:#48e07a; color:#48e07a; }
.del-btn { flex-shrink:0; padding:4px 7px; color:#ff8080; border-color:rgba(255,100,100,0.35); }

.row { margin-bottom:6px; }
.row input { width:100%; box-sizing:border-box; background:rgba(255,255,255,0.06); border:1px solid rgba(255,255,255,0.15); color:#cfd3e6; border-radius:4px; padding:6px 8px; font-size:12px; }
.add-btn { width:100%; margin-top:2px; padding:6px; font-size:12px; }
hr { border:none; border-top:1px solid rgba(255,255,255,0.1); margin:8px 0; }
.error { font-size:11px; color:#ff8080; word-break:break-word; margin-top:4px; }
</style>
