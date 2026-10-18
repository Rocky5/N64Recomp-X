'use strict';
// Runs n64rx (tools/n64rx/n64rx.py) and reads what it says. The app never
// does a build step itself: it starts the tool with an argument array (no
// shell), turns its JSON lines into events, and can stop the whole process
// tree.
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const { LineParser } = require('./jsonlines');

const IS_WIN = process.platform === 'win32';

const TASKKILL = path.join(process.env.SystemRoot || process.env.windir || 'C:\\Windows', 'System32', 'taskkill.exe');

/** kill a child and everything it started (N64Recomp, clang, ninja, bash...) */
function killTree(child) {
  if (!child || child.exitCode !== null || (child.killed && child.signalCode)) return;
  if (IS_WIN) {
    spawnSync(TASKKILL, ['/pid', String(child.pid), '/T', '/F'], { windowsHide: true });
    return;
  }
  try {
    process.kill(-child.pid, 'SIGTERM');
  } catch { /* already gone */ }
  const t = setTimeout(() => {
    try {
      process.kill(-child.pid, 'SIGKILL');
    } catch { /* gone */ }
  }, 3000);
  t.unref();
}

/** argv for the log: a password value never written */
function redact(args) {
  const out = [];
  for (let i = 0; i < args.length; i++) {
    const a = String(args[i]);
    if (/^--pass(word)?=/.test(a)) out.push(a.replace(/=.*/, '=***'));
    else if (/^--pass(word)?$/.test(a) && i + 1 < args.length) {
      out.push(a, '***');
      i++;
    } else out.push(a);
  }
  return out;
}

function spawnTool(run, toolArgs, env = null) {
  if (!run.cmd) {
    const e = new Error('Python 3.11 or newer was not found on this computer.');
    e.hint = 'Install Python from https://www.python.org (tick "Add to PATH"), or set N64RX_PYTHON to its path, then start the app again.';
    throw e;
  }
  return spawn(run.cmd, [...run.args, ...toolArgs], {
    cwd: fs.existsSync(run.cwd) ? run.cwd : undefined,
    env: env ? { ...run.env, ...env } : run.env,
    windowsHide: true,
    detached: !IS_WIN,   // its own process group, so kill(-pid) reaches the whole tree
    stdio: ['ignore', 'pipe', 'pipe'],
  });
}

/**
 * `n64rx --events json <sub> ...args`. Returns {child, cancel, done}; done
 * resolves {code, result, error, cancelled, lines, logFile}. Every event
 * (and every non-JSON line, as {ev:'log'}) goes to onEvent.
 */
function runEvents(run, sub, args, { onEvent = () => {}, logFile = null, env = null } = {}) {
  const toolArgs = ['--events', 'json', sub, ...args];
  const lines = [];
  let log = null;
  if (logFile) {
    fs.mkdirSync(path.dirname(logFile), { recursive: true });
    log = fs.createWriteStream(logFile);
    log.write(`n64rx ${redact(toolArgs).map((a) => (/\s/.test(a) ? JSON.stringify(a) : a)).join(' ')}\n(${run.mode})\n\n`);
  }
  let result = null;
  let error = null;
  let cancelled = false;
  const handle = (ev) => {
    if (ev.ev === 'result') result = ev;
    if (ev.ev === 'error') error = ev;
    const text = ev.ev === 'log' ? ev.text : ev.ev === 'step' ? `[${ev.n}/${ev.of}] ${ev.text}`
      : ev.ev === 'note' ? `      ${ev.text}` : ev.ev === 'done' ? `      done in ${ev.secs} s`
        : ev.ev === 'error' ? `n64rx: ${ev.message}${ev.hint ? `\nWhat to do: ${ev.hint}` : ''}` : JSON.stringify(ev);
    lines.push(text);
    if (lines.length > 5000) lines.splice(0, lines.length - 5000);
    if (log) log.write(text + '\n');
    try {
      onEvent(ev);
    } catch { /* a listener's problem is not the build's */ }
  };
  let child;
  try {
    child = spawnTool(run, toolArgs, env);
  } catch (e) {
    const err = { ev: 'error', message: e.message, hint: e.hint || null };
    handle(err);
    if (log) log.end();
    return { child: null, cancel() {}, done: Promise.resolve({ code: 2, result: null, error: err, cancelled: false, lines, logFile }) };
  }
  const out = new LineParser(handle, 'stdout');
  const errp = new LineParser(handle, 'stderr');
  child.stdout.on('data', (c) => out.push(c));
  child.stderr.on('data', (c) => errp.push(c));
  const done = new Promise((resolve) => {
    let spawnError = null;
    child.on('error', (e) => { spawnError = e; });
    child.on('close', (code, signal) => {
      out.end();
      errp.end();
      if (spawnError && !error) {
        handle({ ev: 'error', message: `Could not start ${run.cmd} (${spawnError.code || spawnError.message}).`,
          hint: 'Check that Python is installed, or set N64RX_PYTHON to its path.' });
      }
      if (!cancelled && !error && code !== 0) {
        // a plain-text failure (n64rx printed "n64rx: ..." and "What to do: ...")
        error = fromText(lines) || { ev: 'error', message: `n64rx stopped (exit code ${code ?? signal}).`, hint: 'Open the log for the details.' };
        handle(error);
      }
      if (log) log.end();
      resolve({ code: code ?? -1, result, error: cancelled ? null : error, cancelled, lines, logFile });
    });
  });
  return {
    child,
    cancel() {
      cancelled = true;
      killTree(child);
    },
    done,
  };
}

/** "n64rx: <message>" / "What to do: <hint>" from the text output */
function fromText(lines) {
  let message = null;
  let hint = null;
  for (const l of lines) {
    const m = /^n64rx: (.*)$/.exec(l.trim());
    if (m) message = m[1];
    const h = /^What to do: (.*)$/.exec(l.trim());
    if (h) hint = h[1];
  }
  return message ? { ev: 'error', message, hint } : null;
}

/** the first JSON object in a text (n64rx info --json prints one) */
function firstJson(text) {
  const t = text.trim();
  try {
    return JSON.parse(t);
  } catch { /* look inside */ }
  const a = t.indexOf('{');
  const b = t.lastIndexOf('}');
  if (a >= 0 && b > a) {
    try {
      return JSON.parse(t.slice(a, b + 1));
    } catch { /* no */ }
  }
  return null;
}

/** `n64rx info <rom> --json` -> {ok, info} | {ok:false, error:{message, hint}} */
function info(run, rom, { timeoutMs = 120000 } = {}) {
  return new Promise((resolve) => {
    let child;
    try {
      child = spawnTool(run, ['info', rom, '--json']);
    } catch (e) {
      resolve({ ok: false, error: { message: e.message, hint: e.hint || null } });
      return;
    }
    let out = '';
    let err = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (c) => { out += c; });
    child.stderr.on('data', (c) => { err += c; });
    const timer = setTimeout(() => killTree(child), timeoutMs);
    child.on('error', (e) => {
      clearTimeout(timer);
      resolve({ ok: false, error: { message: `Could not start ${run.cmd} (${e.code || e.message}).`, hint: 'Check that Python is installed, or set N64RX_PYTHON to its path.' } });
    });
    child.on('close', (code) => {
      clearTimeout(timer);
      const obj = code === 0 ? firstJson(out) : null;
      if (obj && obj.rom && obj.compat) {
        resolve({ ok: true, info: obj });
        return;
      }
      const all = (out + '\n' + err).split(/\r?\n/);
      const ev = all.map((l) => {
        try {
          const o = JSON.parse(l);
          return o && o.ev === 'error' ? o : null;
        } catch {
          return null;
        }
      }).find(Boolean);
      const e = ev || fromText(all) || {
        message: code === 0 ? 'n64rx did not describe this ROM.' : `n64rx could not read this ROM (exit code ${code}).`,
        hint: (err.trim().split(/\r?\n/).slice(-3).join(' ') || 'Check that the file is an N64 ROM (.z64, .n64 or .v64).'),
      };
      resolve({ ok: false, error: { message: e.message, hint: e.hint || null } });
    });
  });
}

module.exports = { runEvents, info, killTree, firstJson, fromText, redact };
