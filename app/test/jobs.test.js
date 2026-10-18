'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { Jobs } = require('../src/jobs');

const later = (ms, v) => new Promise((r) => setTimeout(() => r(v), ms));
function fakeHandle(ms = 30) {
  let cancel;
  const done = new Promise((r) => {
    const t = setTimeout(() => r({ code: 0, result: { ok: 1 }, error: null, cancelled: false }), ms);
    cancel = () => {
      clearTimeout(t);
      r({ code: -1, result: null, error: null, cancelled: true });
    };
  });
  return { done, cancel: () => cancel() };
}

test('a double call (a double click on Build, or Build + Send) runs one process, not two', async () => {
  const jobs = new Jobs();
  let started = 0;
  const start = () => {
    started++;
    return fakeHandle();
  };
  const getRunner = () => later(20, { mode: 'fake' });   // the await that used to let the second call in
  const a = jobs.run('build', getRunner, start);
  await assert.rejects(jobs.run('deploy', getRunner, start), /already running/);
  assert.equal((await a).code, 0);
  assert.equal(started, 1);
  assert.equal(jobs.busy(), false);
});

test('the slot is released when getting the runner or starting fails', async () => {
  const jobs = new Jobs();
  await assert.rejects(jobs.run('build', () => Promise.reject(new Error('no source')), fakeHandle), /no source/);
  assert.equal(jobs.busy(), false);
  await assert.rejects(jobs.run('build', async () => ({}), () => { throw new Error('spawn'); }), /spawn/);
  assert.equal(jobs.busy(), false);
});

test('cancel before the process exists means it never starts', async () => {
  const jobs = new Jobs();
  let started = 0;
  const p = jobs.run('build', () => later(20, {}), () => {
    started++;
    return fakeHandle(5000);
  });
  jobs.cancel();
  const r = await p;
  assert.equal(r.cancelled, true);
  assert.equal(started, 0);
});

test('a job and the toolchain download exclude each other', async () => {
  const jobs = new Jobs();
  const slot = jobs.claimToolchain(() => {});
  await assert.rejects(jobs.run('build', async () => ({}), fakeHandle), /downloading/);
  jobs.release(slot);
  const p = jobs.run('build', () => later(10, {}), () => fakeHandle(30));
  assert.throws(() => jobs.claimToolchain(() => {}), /build is running/);
  await p;
  assert.doesNotThrow(() => jobs.release(jobs.claimToolchain(() => {})));
});
