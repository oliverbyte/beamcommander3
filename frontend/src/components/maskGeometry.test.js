// Unit tests for maskGeometry.js (safety mask editing in the Zoning panel).
// Run with `npm test` - also runs before every push (.githooks/pre-push), so a
// failing test aborts the push.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  MASK_MIN_SIZE, MAX_MASKS, newMask, normalizeMask, moveMask, resizeMask, maskToPx, withoutOutlines,
} from './maskGeometry.js'

const near = (a, b, eps = 1e-9) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`)
const valid = (m) => {
  for (const k of ['x0', 'y0', 'x1', 'y1']) assert.ok(m[k] >= -1 && m[k] <= 1 && Number.isFinite(m[k]), `${k}=${m[k]}`)
  assert.ok(m.x0 <= m.x1 && m.y0 <= m.y1)
}

test('newMask is valid, fully blanking and has its outline off', () => {
  for (let i = 0; i < 20; i++) {
    const m = newMask(i)
    valid(m)
    assert.equal(m.reduce, 100)
    assert.equal(m.outline, false)
  }
  assert.notDeepEqual(newMask(0), newMask(1)) // repeated adds don't stack exactly
})

test('normalizeMask orders corners, clamps ranges and repairs garbage', () => {
  const m = normalizeMask({ x0: 0.5, y0: 2, x1: -3, y1: -0.2, reduce: 250.4, outline: 1 })
  assert.deepEqual(m, { x0: -1, y0: -0.2, x1: 0.5, y1: 1, reduce: 100, outline: true })
  const g = normalizeMask({ x0: NaN, y0: 'abc', x1: Infinity, y1: undefined, reduce: -5 })
  valid(g)
  assert.equal(g.reduce, 0)
  assert.equal(normalizeMask({ reduce: 'x' }).reduce, 100) // unknown -> safest default
  assert.equal(normalizeMask({ reduce: 33.6 }).reduce, 34)
})

test('moveMask keeps size and stays inside -1..1', () => {
  const m = { x0: -0.2, y0: -0.1, x1: 0.2, y1: 0.3, reduce: 50, outline: true }
  const a = moveMask(m, 0.3, -0.2)
  near(a.x0, 0.1); near(a.x1, 0.5); near(a.y0, -0.3); near(a.y1, 0.1)
  assert.equal(a.reduce, 50); assert.equal(a.outline, true)
  for (const [dx, dy] of [[5, 5], [-5, -5], [5, -5], [-5, 5]]) {
    const b = moveMask(m, dx, dy)
    valid(b)
    near(b.x1 - b.x0, 0.4, 1e-12); near(b.y1 - b.y0, 0.4, 1e-12)
  }
  assert.deepEqual(moveMask(m, 0, 0), m)
})

test('resizeMask moves only the dragged edges', () => {
  const m = { x0: -0.5, y0: -0.5, x1: 0.5, y1: 0.5, reduce: 100, outline: false }
  const r = resizeMask(m, 'r', 0.2, 0.9)
  near(r.x1, 0.7); assert.equal(r.x0, -0.5); assert.equal(r.y0, -0.5); assert.equal(r.y1, 0.5)
  const l = resizeMask(m, 'l', 0.2, 0)
  near(l.x0, -0.3); assert.equal(l.x1, 0.5)
  const t = resizeMask(m, 't', 0, -0.1) // top is +y
  near(t.y1, 0.4); assert.equal(t.y0, -0.5)
  const b = resizeMask(m, 'b', 0, -0.3)
  near(b.y0, -0.8); assert.equal(b.y1, 0.5)
  const tl = resizeMask(m, 'tl', -0.1, 0.1)
  near(tl.x0, -0.6); near(tl.y1, 0.6); assert.equal(tl.x1, 0.5); assert.equal(tl.y0, -0.5)
  const br = resizeMask(m, 'br', 0.1, -0.1)
  near(br.x1, 0.6); near(br.y0, -0.6)
})

test('resizeMask never inverts, collapses below the minimum or leaves the range', () => {
  const m = { x0: -0.5, y0: -0.5, x1: 0.5, y1: 0.5, reduce: 100, outline: false }
  for (const h of ['l', 'r', 't', 'b', 'tl', 'tr', 'bl', 'br']) {
    for (const d of [-10, -1, -0.99, 0.99, 1, 10]) {
      const r = resizeMask(m, h, d, d)
      valid(r)
      assert.ok(r.x1 - r.x0 >= MASK_MIN_SIZE - 1e-12, `${h} ${d} width`)
      assert.ok(r.y1 - r.y0 >= MASK_MIN_SIZE - 1e-12, `${h} ${d} height`)
    }
  }
})

test('maskToPx maps output space onto the stage (y up -> screen down)', () => {
  assert.deepEqual(maskToPx({ x0: -1, y0: -1, x1: 1, y1: 1 }, 200), { left: 0, top: 0, width: 200, height: 200 })
  assert.deepEqual(maskToPx({ x0: 0, y0: 0, x1: 0.5, y1: 0.5 }, 200), { left: 100, top: 50, width: 50, height: 50 })
  // a mask at the bottom of the laser range is drawn at the bottom of the stage
  const low = maskToPx({ x0: -0.2, y0: -1, x1: 0.2, y1: -0.6 }, 200)
  near(low.top + low.height, 200)
})

test('withoutOutlines switches every outline off, or returns null if nothing to do', () => {
  assert.equal(withoutOutlines([]), null)
  assert.equal(withoutOutlines(undefined), null)
  assert.equal(withoutOutlines([{ outline: false }]), null)
  const src = [{ x0: 0, outline: true }, { x0: 1, outline: false }]
  const out = withoutOutlines(src)
  assert.deepEqual(out, [{ x0: 0, outline: false }, { x0: 1, outline: false }])
  assert.equal(src[0].outline, true) // input untouched
})

test('MAX_MASKS matches the backend limit', async () => {
  const { readFile } = await import('node:fs/promises')
  const header = await readFile(new URL('../../../backend/output_pipeline.h', import.meta.url), 'utf8')
  const m = header.match(/MAX_MASKS_PER_LASER\s*=\s*(\d+)/)
  assert.ok(m, 'MAX_MASKS_PER_LASER not found in output_pipeline.h')
  assert.equal(MAX_MASKS, Number(m[1]))
})
