'use strict';
// The renderer's whole API: a handful of calls, each checked again in main.js.
const { contextBridge, ipcRenderer, webUtils } = require('electron');

const call = (ch) => (...a) => ipcRenderer.invoke(ch, ...a);

function on(channel) {
  return (fn) => {
    const h = (_e, payload) => fn(payload);
    ipcRenderer.on(channel, h);
    return () => ipcRenderer.removeListener(channel, h);
  };
}

contextBridge.exposeInMainWorld('n64rx', {
  platform: process.platform,
  about: call('app:about'),
  pickRom: call('rom:pick'),
  pickLearn: call('learn:pick'),
  pickOut: call('out:pick'),
  pickIso: call('iso:pick'),
  pathForFile: (file) => {
    try {
      return webUtils.getPathForFile(file) || null;
    } catch {
      return null;
    }
  },
  info: call('rom:info'),
  build: call('build:start'),
  deploy: call('deploy:start'),
  cancel: call('job:cancel'),
  showFolder: call('shell:show'),
  openLogDir: call('shell:openLogDir'),
  openRepo: call('shell:openRepo'),
  openLicense: call('shell:openLicense'),
  copyLog: call('log:copy'),
  data: { info: call('data:info'), open: call('data:open'), remove: call('data:remove') },
  settings: { get: call('settings:get'), set: call('settings:set') },
  toolchain: { status: call('toolchain:status'), install: call('toolchain:install'), useSystem: call('toolchain:useSystem') },
  onEvent: on('n64rx:event'),
  onToolchainProgress: on('toolchain:progress'),
});
