// Unit tests for showFx.js - the preview's flicker must match the daemon's
// (backend/show_fx.h). Run with `npm test` / before every push.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { flickerIsLit, createClockSync, FLICKER_ON_MS_MAX } from './showFx.js'

const litFraction = (hz, onMs, seconds = 10) => {
  const n = 200000; let lit = 0
  for (let i = 0; i < n; i++) lit += flickerIsLit((seconds * i) / n, hz, onMs) ? 1 : 0
  return lit / n
}
const near = (a, b, eps) => assert.ok(Math.abs(a - b) <= eps, `${a} != ${b}`)

test('off and garbage are always lit', () => {
  for (const t of [0, 0.3, 1e6]) {
    assert.equal(flickerIsLit(t, 0, 0), true)
    assert.equal(flickerIsLit(t, -2, 50), true)
    assert.equal(flickerIsLit(t, NaN, 50), true)
    assert.equal(flickerIsLit(NaN, 10, 50), true)
  }
})

test('auto on time = half the period, lit at the start of each period', () => {
  near(litFraction(10, 0), 0.5, 0.002)
  assert.equal(flickerIsLit(0.049, 10, 0), true)
  assert.equal(flickerIsLit(0.051, 10, 0), false)
})

test('on time sets the lit duration, same values as the C++ tests', () => {
  near(litFraction(10, 20), 0.2, 0.002)
  near(litFraction(4, 50), 0.2, 0.002)
  near(litFraction(1, 900), 0.9, 0.002)
  near(litFraction(10, 100), 1, 1e-9)
  near(litFraction(0.5, 5000, 20), 0.5, 0.002) // capped at FLICKER_ON_MS_MAX
  assert.equal(flickerIsLit(0.019, 10, 20), true)
  assert.equal(flickerIsLit(0.021, 10, 20), false)
  assert.equal(flickerIsLit(-0.099, 10, 20), true)
  assert.equal(flickerIsLit(-0.05, 10, 20), false)
})

test('on time cap matches the backend', async () => {
  const { readFile } = await import('node:fs/promises')
  const header = await readFile(new URL('../../../backend/show_fx.h', import.meta.url), 'utf8')
  const m = header.match(/kFlickerOnMsMax\s*=\s*([\d.]+)f?/)
  assert.ok(m)
  assert.equal(FLICKER_ON_MS_MAX, Number(m[1]))
})

test('clock sync keeps the least delayed sample and maps client to server time', () => {
  const c = createClockSync()
  assert.equal(c.toServer(5), 5) // no sample yet
  c.update(100.0, 10.0)  // offset 90
  c.update(100.02, 10.05) // arrived 30 ms later than ideal -> 89.97, ignored (decays only a little)
  near(c.offset, 89.9995, 1e-9)
  c.update(101.0, 10.9) // 90.1 - better sample wins
  near(c.offset, 90.1, 1e-9)
  near(c.toServer(11), 101.1, 1e-9)
  c.update(5.0, 11.0) // daemon restarted: jump
  near(c.offset, -6, 1e-9)
})
