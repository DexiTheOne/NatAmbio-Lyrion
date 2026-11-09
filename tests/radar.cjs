const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
    '../plugin/HTML/EN/plugins/NatAmbioAlpha/radar.js'), 'utf8');
const makeCloudTarget = vm.runInNewContext(`${source}\nmakeCloudTarget`, {
    document: {
        querySelector: () => ({getContext() {}, addEventListener() {}}),
        querySelectorAll: () => [],
    },
    // Leave the initial player request pending so no dashboard is needed.
    fetch: () => new Promise(() => {}),
    AbortSignal: {timeout() {}},
    setTimeout() {},
    requestAnimationFrame() {},
});

const sample = {rate: 48000, valid: true, major: .01, minor: 0};
const diagonal = Math.SQRT1_2;
const left = makeCloudTarget({...sample, mid: diagonal, side: diagonal});
const right = makeCloudTarget({...sample, mid: diagonal, side: -diagonal});
assert.equal(left.angle, -Math.PI / 4);
assert.equal(right.angle, Math.PI / 4);
assert.equal(makeCloudTarget({...sample, mid: -diagonal, side: -diagonal}).angle, left.angle);
assert.ok(Math.abs(makeCloudTarget({...sample, mid: 1, side: 0}).angle) === 0);

const diffuse = makeCloudTarget({...sample, mid: diagonal, side: diagonal, minor: .00998});
assert.ok(Math.abs(diffuse.angle) === 0 && diffuse.spread > .99);
const antiphase = makeCloudTarget({...sample, mid: 0, side: 1});
assert.ok(Math.abs(antiphase.angle) === 0 && antiphase.spread === 1);

assert.equal(left.opacity, 1);
const quiet = makeCloudTarget({...sample, mid: diagonal, side: diagonal, major: 2e-6});
assert.equal(quiet.opacity, .5);
assert.equal(makeCloudTarget({...sample, mid: diagonal, side: diagonal, major: 1e-8}).opacity, 0);
assert.equal(makeCloudTarget({...sample, major: 0}).opacity, 0);
assert.equal(makeCloudTarget({...sample, rate: 0}).opacity, 0);
