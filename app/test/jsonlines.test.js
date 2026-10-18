'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { LineParser, parseLine } = require('../src/jsonlines');

function collect() {
  const evs = [];
  return { evs, p: new LineParser((e) => evs.push(e)) };
}

test('events split across chunks are put back together', () => {
  const { evs, p } = collect();
  const line = JSON.stringify({ ev: 'step', n: 2, of: 5, text: 'Recompiling the game\'s code (N64Recomp)' }) + '\n';
  for (const ch of [line.slice(0, 7), line.slice(7, 30), line.slice(30)]) p.push(Buffer.from(ch));
  assert.deepEqual(evs, [{ ev: 'step', n: 2, of: 5, text: 'Recompiling the game\'s code (N64Recomp)' }]);
});

test('a UTF-8 character split between chunks survives', () => {
  const { evs, p } = collect();
  const b = Buffer.from(JSON.stringify({ ev: 'note', text: 'Pokémon Snap' }) + '\n');
  const at = b.indexOf(0xc3) + 1;   // between the two bytes of é
  p.push(b.subarray(0, at));
  p.push(b.subarray(at));
  assert.equal(evs[0].text, 'Pokémon Snap');
});

test('non-JSON lines become log events; CRLF and blank lines are handled', () => {
  const { evs, p } = collect();
  p.push('[ 12%] Building C object foo.o\r\n\r\n{"ev":"done","secs":1.5}\r\n{not json}\n{"no":"ev"}\n');
  assert.deepEqual(evs, [
    { ev: 'log', text: '[ 12%] Building C object foo.o', stream: 'stdout' },
    { ev: 'done', secs: 1.5 },
    { ev: 'log', text: '{not json}', stream: 'stdout' },
    { ev: 'log', text: '{"no":"ev"}', stream: 'stdout' },
  ]);
});

test('a last line without a newline is flushed at the end', () => {
  const { evs, p } = collect();
  p.push('{"ev":"result","folder":"C:/out/Pilotwings 64","status":"Status: Gold"}');
  assert.equal(evs.length, 0);
  p.end();
  assert.equal(evs[0].ev, 'result');
  assert.equal(evs[0].folder, 'C:/out/Pilotwings 64');
});

test('unknown event names are logs, error events keep message and hint', () => {
  assert.equal(parseLine('{"ev":"weird"}').ev, 'log');
  assert.deepEqual(parseLine('{"ev":"error","message":"m","hint":"h"}'), { ev: 'error', message: 'm', hint: 'h' });
  assert.equal(parseLine('plain', 'stderr').stream, 'stderr');
});
