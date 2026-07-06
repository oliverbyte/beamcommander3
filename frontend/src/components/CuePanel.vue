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
        >
          <svg v-if="cues[n]" class="cue-preview" viewBox="0 0 24 24">
            <defs v-if="cues[n].rainbow_amount > 0">
              <!-- userSpaceOnUse (fixed 0..24 icon coordinates), not the
                   default objectBoundingBox: a horizontal "line" shape has
                   a zero-height bounding box, and objectBoundingBox
                   gradients silently fail to paint at all on a degenerate
                   (zero width/height) bounding box - the gradient just
                   didn't render for that shape. Fixed coordinates sidestep
                   that entirely and work the same for every shape. -->
              <linearGradient :id="'cue-rainbow-' + n" gradientUnits="userSpaceOnUse" x1="0" y1="12" x2="24" y2="12">
                <stop offset="0%"    stop-color="hsl(0,100%,55%)" />
                <stop offset="16.6%" stop-color="hsl(60,100%,55%)" />
                <stop offset="33.3%" stop-color="hsl(120,100%,55%)" />
                <stop offset="50%"   stop-color="hsl(180,100%,55%)" />
                <stop offset="66.6%" stop-color="hsl(240,100%,55%)" />
                <stop offset="83.3%" stop-color="hsl(300,100%,55%)" />
                <stop offset="100%"  stop-color="hsl(360,100%,55%)" />
              </linearGradient>
            </defs>
            <g :style="cuePosStyle(n)">
              <g class="cue-move-group" :style="cueMoveStyle(n)">
              <g class="cue-spin-group" :style="cueRotateStyle(n)">
                <circle v-if="cues[n].shape === 'circle'" cx="12" cy="12" r="8"
                  fill="none" :stroke="cueStroke(n)" stroke-width="2" :stroke-dasharray="cueDashArray(n)" :style="cueShapeStyle(n)" />
                <line v-else-if="cues[n].shape === 'line'" x1="3" y1="12" x2="21" y2="12"
                  :stroke="cueStroke(n)" stroke-width="2" :stroke-dasharray="cueDashArray(n)" :style="cueShapeStyle(n)" />
                <polygon v-else-if="cues[n].shape === 'triangle'" points="12,4 20,20 4,20"
                  fill="none" :stroke="cueStroke(n)" stroke-width="2" :stroke-dasharray="cueDashArray(n)" :style="cueShapeStyle(n)" />
                <rect v-else-if="cues[n].shape === 'square'" x="4" y="4" width="16" height="16"
                  fill="none" :stroke="cueStroke(n)" stroke-width="2" :stroke-dasharray="cueDashArray(n)" :style="cueShapeStyle(n)" />
                <path v-else-if="cues[n].shape === 'wave'" d="M-6,12 Q-1.5,4 3,12 T12,12 T21,12 T30,12"
                  fill="none" :stroke="cueStroke(n)" stroke-width="2" :stroke-dasharray="cueDashArray(n)" :style="cueShapeStyle(n, 'wave')" />
                <path v-else d="M3,12 Q7.5,4 12,12 T21,12"
                  fill="none" :stroke="cueStroke(n)" stroke-width="2" :stroke-dasharray="cueDashArray(n)" :style="cueShapeStyle(n)" />
              </g>
              </g>
            </g>
          </svg>
          <span class="cue-num" :class="{ empty: !cues[n] }">{{ n }}</span>
        </button>
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

// Light preview shown right on each populated cue button: a small SVG icon
// matching the saved shape, in the saved beam color, with a dashed stroke
// standing in for "dotted" cues (dot_amount < 1) - so the expected output
// is visible at a glance in the overview, without needing to recall the
// cue first. Reads straight off `cues[n]` (the full saved state object),
// which is refreshed as soon as saveCue()'s save+re-fetch resolves, so a
// freshly saved cue's preview appears immediately.
//
// Also previews the animated/positional parameters that a static icon
// can't show as a plain color+outline: pos_x/pos_y (static offset of the
// whole icon), rotation_speed (spins the icon, direction matches sign),
// rainbow (animates the stroke hue when rainbow_amount+rainbow_speed are
// both set), and dot_amount (dashed stroke standing in for "dotted" cues).
// Deliberately NOT animated: dot positions don't actually move/flicker in
// the real output unless rotation_speed or move_mode says so (scan rate
// only affects how often the same static picture gets redrawn, which is
// invisible to the eye) - an earlier version animated the dash offset to
// hint at scan rate, but that made static dotted cues look like they were
// travelling/flickering when the real hardware output is steady.
function cueColor(n) {
  const c = cues[n]
  if (!c) return '#48e07a'
  const to = v => Math.round(Math.max(0, Math.min(1, v)) * 255)
  return `rgb(${to(c.r)},${to(c.g)},${to(c.b)})`
}
// Rainbow cues get a multi-hue gradient stroke (see the per-button
// <linearGradient> in the template) instead of the flat beam color -
// rainbow_amount fully replaces the color in the real output with a
// spatial hue band across the shape, so a single flat hue looked nothing
// like the actual 3D preview/hardware output.
function cueStroke(n) {
  const c = cues[n]
  if (c && c.rainbow_amount > 0) return `url(#cue-rainbow-${n})`
  return cueColor(n)
}
function cueDashArray(n) {
  const c = cues[n]
  // Only genuinely dotted cues (dot_amount < 1) get a dashed stroke - a
  // solid cue's real output is one continuous line, so its icon has to
  // stay solid too, not just visually thinner dashes.
  return c && c.dot_amount < 1 ? '2,2' : 'none'
}
// Static x/y shift: pos_x/pos_y are normalised -1..1, mapped to a small
// pixel offset within the 24-unit viewBox (SVG y grows downward, so pos_y
// - "up" in the real output - has to flip sign here).
function cuePosStyle(n) {
  const c = cues[n]
  if (!c) return {}
  const tx = (c.pos_x || 0) * 6
  const ty = -(c.pos_y || 0) * 6
  return { transform: `translate(${tx}px, ${ty}px)` }
}
function cueRotateStyle(n) {
  const c = cues[n]
  const speed = c && c.rotation_speed
  if (!speed) return {}
  const dur = 1 / Math.max(0.02, Math.abs(speed))
  return { animation: `cue-spin ${dur}s linear infinite`, animationDirection: speed < 0 ? 'reverse' : 'normal' }
}
// move_mode (pan/circle/tilt/eight/random) previously had no preview at all
// - only the static pos_x/pos_y offset showed, so a cue like #6 (pan) just
// looked frozen in place instead of sweeping back and forth like the real
// output does. Each mode gets its own small looping keyframe animation
// (see the unscoped <style> block) built from the same shapes as the
// backend's calc_movement(), driven by a CSS custom property (--amp) so
// the amplitude still reflects the cue's saved move_size, and a duration
// from move_speed (cycles/sec, like rotation_speed).
const MOVE_ANIM = { pan: 'cue-move-pan', tilt: 'cue-move-tilt', circle: 'cue-move-circle', eight: 'cue-move-eight', random: 'cue-move-random' }
function cueMoveStyle(n) {
  const c = cues[n]
  const anim = c && MOVE_ANIM[c.move_mode]
  if (!anim || !c.move_speed) return {}
  const amp = Math.min(6, (c.move_size || 0) * 3)
  const dur = 1 / Math.max(0.02, Math.abs(c.move_speed))
  return { '--amp': `${amp}px`, animation: `${anim} ${dur}s linear infinite` }
}
function cueShapeStyle(n, shape) {
  const c = cues[n]
  if (!c) return {}
  const anims = []
  // Animate the gradient's hue for any nonzero rainbow_speed (sign only
  // affects direction, which a full 360 deg hue loop makes visually
  // symmetric either way, so it's not worth branching on here).
  if (c.rainbow_amount > 0 && c.rainbow_speed) anims.push(`cue-hue ${1 / Math.abs(c.rainbow_speed)}s linear infinite`)
  // Dotted cues only visibly strobe/chase in the real output when the scan
  // rate is low enough that the same dot isn't revisited faster than the
  // eye's flicker-fusion threshold - verified directly against the live
  // WS preview: a dotted cue at 30kpps looks completely static (matches
  // the earlier fix removing the fake travel animation), but the same
  // shape at 1kpps clearly shows individual dots flashing/chasing one at a
  // time instead of a steady picture. Below ~5kpps, reinstate a slow
  // dash-travel animation (duration scaled to the rate) to preview that
  // real strobing instead of falsely looking rock-solid.
  if (c.dot_amount < 1 && c.rate_kpps < 5) {
    const dur = Math.max(0.4, 3 / Math.max(0.1, c.rate_kpps))
    anims.push(`cue-dash ${dur}s linear infinite`)
  }
  // "wave" animates its phase over time in the real output (driven by
  // wave_speed) - "staticwave" deliberately doesn't (matches the backend:
  // only "wave" integrates a time-based phase), so only give the wave
  // shape's path a travelling copy of itself to preview that motion.
  if (shape === 'wave' && c.wave_speed) {
    const wdur = 1 / Math.max(0.05, Math.abs(c.wave_speed))
    anims.push(`cue-wave-travel ${wdur}s linear infinite ${c.wave_speed < 0 ? 'reverse' : 'normal'}`)
  }
  return { animation: anims.join(', ') }
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
  position:relative; aspect-ratio:1; background:rgba(255,255,255,0.05);
  border:1px solid rgba(255,255,255,0.15);
  color:#6a7090; border-radius:6px; font-size:13px; cursor:pointer;
  padding:0; overflow:hidden;
}
.cue-btn:hover { background:rgba(255,255,255,0.12); }
.cue-btn.populated { background:rgba(20,22,32,0.9); border-color:#48e07a; color:#48e07a; }
.cue-btn.armed { border-color:#ff4040; }
.cue-btn.armed:hover { background:rgba(255,60,60,0.2); }
.cue-btn.move-armed { border-color:rgba(143,227,255,0.5); }
.cue-btn.move-armed:hover { background:rgba(143,227,255,0.2); }
.cue-btn.move-source { background:rgba(143,227,255,0.3); border-color:#8fe3ff; color:#8fe3ff; }
.cue-preview {
  position:absolute; inset:0; width:100%; height:100%;
  filter:drop-shadow(0 0 3px currentColor);
}
.cue-spin-group { transform-box:fill-box; transform-origin:center; }
.cue-num {
  position:absolute; bottom:1px; right:3px; font-size:9px; line-height:1;
  color:rgba(207,211,230,0.75); text-shadow:0 0 2px rgba(0,0,0,0.9);
  pointer-events:none;
}
.cue-num.empty {
  inset:0; bottom:auto; right:auto;
  display:flex; align-items:center; justify-content:center;
  width:100%; height:100%; font-size:13px; color:inherit; text-shadow:none;
}
.hint { font-size:10px; color:#6a7090; margin:8px 0 0; line-height:1.4; }
</style>

<!--
  Unscoped on purpose: Vue's scoped-CSS compiler rewrites @keyframes names
  declared inside <style scoped> (to avoid cross-component name collisions),
  but the animation names referenced above are plain strings built in JS
  (cueRotateStyle/cueShapeStyle) and can't be rewritten to match - so those
  animations would silently never run if these keyframes stayed scoped.
-->
<style>
@keyframes cue-spin { from { transform:rotate(0deg); } to { transform:rotate(360deg); } }
@keyframes cue-hue  { from { filter:hue-rotate(0deg); } to { filter:hue-rotate(360deg); } }
@keyframes cue-dash { from { stroke-dashoffset:0; } to { stroke-dashoffset:-24; } }
@keyframes cue-wave-travel { from { transform:translateX(0); } to { transform:translateX(-18px); } }
/* Movement previews - shapes mirror calc_movement() in laser_daemon.cpp
   (pan: x=sin, tilt: y=sin, circle: cos/sin, eight: sin/sin(2x)), sampled
   at quarter-phase points since that's exactly where sin/cos hit 0/+-1. */
@keyframes cue-move-pan {
  0%, 100% { transform:translateX(0); }
  25%      { transform:translateX(var(--amp)); }
  50%      { transform:translateX(0); }
  75%      { transform:translateX(calc(var(--amp) * -1)); }
}
@keyframes cue-move-tilt {
  0%, 100% { transform:translateY(0); }
  25%      { transform:translateY(calc(var(--amp) * -1)); }
  50%      { transform:translateY(0); }
  75%      { transform:translateY(var(--amp)); }
}
@keyframes cue-move-circle {
  0%, 100% { transform:translate(var(--amp), 0); }
  25%      { transform:translate(0, calc(var(--amp) * -1)); }
  50%      { transform:translate(calc(var(--amp) * -1), 0); }
  75%      { transform:translate(0, var(--amp)); }
}
@keyframes cue-move-eight {
  0%, 50%, 100% { transform:translate(0, 0); }
  25%           { transform:translate(var(--amp), calc(var(--amp) * -1)); }
  75%           { transform:translate(calc(var(--amp) * -1), var(--amp)); }
}
@keyframes cue-move-random {
  0%, 100% { transform:translate(0, 0); }
  20%      { transform:translate(var(--amp), calc(var(--amp) * -0.6)); }
  45%      { transform:translate(calc(var(--amp) * -0.7), var(--amp)); }
  70%      { transform:translate(calc(var(--amp) * 0.5), calc(var(--amp) * 0.8)); }
}
</style>
