// Pure geometry for the Zoning panel's safety mask editor (ZoningPanel.vue).
// Masks are rectangles {x0, y0, x1, y1, reduce, outline} in a laser's
// physical output space: -1..1 on both axes, y pointing *up*, x0 < x1 and
// y0 < y1 - the same model as LaserMask in backend/output_pipeline.h (which
// re-validates everything it receives, so this module is about a sane
// editing experience, not the last line of defence). No Vue/DOM in here so
// it can be unit-tested with `node --test` (see maskGeometry.test.js, run
// before every push).

export const MASK_MIN_SIZE = 0.02 // smallest width/height a drag can produce
export const MAX_MASKS = 64 // must match MAX_MASKS_PER_LASER in output_pipeline.h

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v))

// A sensible new mask: centered, 0.5 x 0.5, fully blanking, outline off -
// offset a little per existing mask so repeated "Add" clicks don't stack
// exactly on top of each other.
export function newMask(existingCount = 0) {
  const off = ((existingCount % 6) * 0.08)
  return normalizeMask({ x0: -0.25 + off, y0: -0.25 - off, x1: 0.25 + off, y1: 0.25 - off, reduce: 100, outline: false })
}

// Returns a valid copy: finite numbers, corners ordered, inside -1..1,
// reduce rounded to whole percent in 0..100, outline boolean.
export function normalizeMask(m) {
  const num = (v, d) => (Number.isFinite(Number(v)) ? Number(v) : d)
  let x0 = clamp(num(m.x0, -0.25), -1, 1), x1 = clamp(num(m.x1, 0.25), -1, 1)
  let y0 = clamp(num(m.y0, -0.25), -1, 1), y1 = clamp(num(m.y1, 0.25), -1, 1)
  if (x0 > x1) [x0, x1] = [x1, x0]
  if (y0 > y1) [y0, y1] = [y1, y0]
  return { x0, y0, x1, y1, reduce: clamp(Math.round(num(m.reduce, 100)), 0, 100), outline: !!m.outline }
}

// Moves a mask by (dx, dy) without changing its size; stops at the -1..1
// border instead of shrinking or leaving the laser's range.
// The delta itself is clamped and added to both edges (rather than
// recomputing one edge as "other edge + size"), so a zero move is exact and
// rounding can't accumulate over many drag steps.
export function moveMask(m, dx, dy) {
  const cdx = clamp(dx, -1 - m.x0, 1 - m.x1)
  const cdy = clamp(dy, -1 - m.y0, 1 - m.y1)
  return {
    ...m,
    x0: clamp(m.x0 + cdx, -1, 1), x1: clamp(m.x1 + cdx, -1, 1),
    y0: clamp(m.y0 + cdy, -1, 1), y1: clamp(m.y1 + cdy, -1, 1),
  }
}

// Resizes ("crops") a mask by dragging one edge or corner. `handle` is any
// combination of 'l'/'r' (x0/x1) and 't'/'b' (y1/y0 - top is +y), e.g.
// 'r', 'tl'. The dragged edge never crosses the opposite one (keeps at
// least MASK_MIN_SIZE) and never leaves -1..1.
export function resizeMask(m, handle, dx, dy) {
  const out = { ...m }
  if (handle.includes('l')) out.x0 = clamp(m.x0 + dx, -1, m.x1 - MASK_MIN_SIZE)
  if (handle.includes('r')) out.x1 = clamp(m.x1 + dx, m.x0 + MASK_MIN_SIZE, 1)
  if (handle.includes('b')) out.y0 = clamp(m.y0 + dy, -1, m.y1 - MASK_MIN_SIZE)
  if (handle.includes('t')) out.y1 = clamp(m.y1 + dy, m.y0 + MASK_MIN_SIZE, 1)
  return out
}

// Stage pixel box (CSS left/top/width/height) for a mask on a square stage
// of `canvas` px representing -1..1 (screen y grows downward).
export function maskToPx(m, canvas) {
  const half = canvas / 2
  return {
    left: half + m.x0 * half,
    top: half - m.y1 * half,
    width: (m.x1 - m.x0) * half,
    height: (m.y1 - m.y0) * half,
  }
}

// Same list with every mask's outline switched off (null if none was on) -
// used when the Zoning panel closes, so the red setup outline never stays
// on the real laser by accident.
export function withoutOutlines(masks) {
  if (!masks || !masks.some((m) => m.outline)) return null
  return masks.map((m) => ({ ...m, outline: false }))
}
