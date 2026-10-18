'use strict';
// Checks on everything the renderer sends over IPC: the main process never
// trusts a path, host or option it did not make itself.
const fs = require('node:fs');
const path = require('node:path');

const ROM_EXT = ['.z64', '.n64', '.v64', '.rom', '.bin'];

class InputError extends Error {
  constructor(message, hint) {
    super(message);
    this.hint = hint || null;
  }
}

function str(v, name, max = 4096) {
  if (typeof v !== 'string' || !v.length || v.length > max || /[\0\r\n]/.test(v)) throw new InputError(`Bad ${name}.`);
  return v;
}

function existingFile(p, name = 'file') {
  str(p, name);
  if (!path.isAbsolute(p)) throw new InputError(`The ${name} path is not absolute.`);
  let st;
  try {
    st = fs.statSync(p);
  } catch {
    throw new InputError(`No ${name} at ${p}.`, 'It may have been moved or deleted: choose it again.');
  }
  if (!st.isFile()) throw new InputError(`${p} is not a file.`);
  return p;
}

function existingDir(p, name = 'folder') {
  str(p, name);
  if (!path.isAbsolute(p)) throw new InputError(`The ${name} path is not absolute.`);
  let st;
  try {
    st = fs.statSync(p);
  } catch {
    throw new InputError(`No ${name} at ${p}.`, 'It may have been moved or deleted.');
  }
  if (!st.isDirectory()) throw new InputError(`${p} is not a folder.`);
  return p;
}

function romFile(p) {
  existingFile(p, 'ROM');
  const st = fs.statSync(p);
  if (st.size > 512 * 1024 * 1024) throw new InputError('This file is too big to be an N64 ROM.', 'Choose the .z64, .n64 or .v64 file itself.');
  return p;
}

/** an IPv4/IPv6 address or a host name: nothing a shell or FTP command could misread */
function host(h) {
  str(h, 'Xbox address', 253);
  const v = h.trim();
  const ipv4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;
  const ipv6 = /^[0-9A-Fa-f:]{2,39}$/;
  const name = /^(?=.{1,253}$)([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/;
  // all digits and dots is an IPv4 address or nothing (256.1.1.1 is not a host name)
  if (ipv4.test(v) || (ipv6.test(v) && v.includes(':')) || (name.test(v) && !/^[\d.]+$/.test(v))) return v;
  throw new InputError(`"${h}" is not an IP address or a host name.`, 'Type the Xbox\'s IP address as its dashboard shows it, e.g. 192.168.1.50.');
}

function ftpField(v, name) {
  if (typeof v !== 'string' || v.length > 128 || /[\0\r\n]/.test(v)) throw new InputError(`Bad ${name}.`);
  return v;
}

function xboxPath(v) {
  str(v, 'folder on the Xbox', 256);
  if (!/^\/[A-Za-z0-9 _./-]*$/.test(v) || v.includes('..')) {
    throw new InputError(`"${v}" is not a folder on the Xbox.`, 'Use a path like /E/Games.');
  }
  return v;
}

function buildOptions(o) {
  if (!o || typeof o !== 'object') throw new InputError('Bad build options.');
  const out = { rom: romFile(o.rom) };
  if (o.iso != null) {
    out.iso = str(o.iso, 'disc image file');
    if (!path.isAbsolute(out.iso) || !/\.iso$/i.test(out.iso)) throw new InputError('The disc image must be an absolute path ending in .iso.');
  } else {
    out.out = str(o.out, 'output folder');
    if (!path.isAbsolute(out.out)) throw new InputError('The output folder path is not absolute.');
  }
  out.video = ['auto', '480', '720'].includes(o.video) ? o.video : 'auto';
  out.fast = o.fast === true;
  out.romBeside = o.romBeside === true;
  out.learn = o.learn ? existingFile(o.learn, 'learn file') : null;
  return out;
}

/** a FATX-safe name, as n64rx's pipeline.folder_name(): 42 characters, none of <>:"/\\|?* */
function fatxName(title) {
  const name = String(title || '').replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').replace(/^[ .]+|[ .]+$/g, '');
  return (name || 'game').slice(0, 42);
}

module.exports = { fatxName, InputError, ROM_EXT, existingFile, existingDir, romFile, host, ftpField, xboxPath, buildOptions };
