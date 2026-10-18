'use strict';
// npm run fake [-- Gold|Silver|Experimental|Unknown]: the app with the fake
// n64rx (test/fake-n64rx.js), for working on the UI without a toolchain.
const { spawn } = require('node:child_process');
const path = require('node:path');

const tier = process.argv[2] || process.env.N64RX_FAKE_TIER || 'Unknown';
const p = spawn(require('electron'), [path.resolve(__dirname, '..')], {
  stdio: 'inherit',
  env: { ...process.env, N64RX_FAKE: '1', N64RX_FAKE_TIER: tier },
});
p.on('close', (c) => process.exit(c ?? 0));
