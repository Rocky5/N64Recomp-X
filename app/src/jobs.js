'use strict';
// One thing at a time: an n64rx job (build, deploy, ISO) or the toolchain
// download, never two. The slot is claimed synchronously, before any await,
// so a double click cannot start a second process; it is released on every
// path out.
const { InputError } = require('./validate');

class Jobs {
  constructor() {
    this.job = null;   // {kind, handle, cancelled}
    this.tc = null;    // {abort}
  }

  busy() {
    return !!(this.job || this.tc);
  }

  claimJob(kind) {
    if (this.job) throw new InputError('Something is already running.', 'Wait for it to finish, or cancel it.');
    if (this.tc) throw new InputError('The build tools are still downloading.', 'Wait for the download to finish, or cancel it.');
    this.job = { kind, handle: null, cancelled: false };
    return this.job;
  }

  claimToolchain(abort) {
    if (this.tc) throw new InputError('The download is already running.');
    if (this.job) throw new InputError('A build is running.', 'Get the build tools when it has finished.');
    this.tc = { abort };
    return this.tc;
  }

  release(slot) {
    if (this.job === slot) this.job = null;
    if (this.tc === slot) this.tc = null;
  }

  /** stop what runs; a job still starting is stopped as soon as its process exists */
  cancel() {
    if (this.job) {
      this.job.cancelled = true;
      if (this.job.handle) this.job.handle.cancel();
    }
    if (this.tc) this.tc.abort();
  }

  /**
   * Run one job: claim (synchronous), get the runner (async), start it with
   * start(run) -> {cancel, done}, wait. Returns done's value, or
   * {cancelled: true} when cancelled before the process started.
   */
  async run(kind, getRunner, start) {
    const slot = this.claimJob(kind);
    try {
      const run = await getRunner();
      if (slot.cancelled) return { cancelled: true, code: -1, result: null, error: null };
      slot.handle = start(run);
      if (slot.cancelled) slot.handle.cancel();
      return await slot.handle.done;
    } finally {
      this.release(slot);
    }
  }
}

module.exports = { Jobs };
