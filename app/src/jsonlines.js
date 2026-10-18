'use strict';
// n64rx --events json writes one JSON object a line. Chunks arrive split
// anywhere (mid-line, mid-UTF-8 character); anything that is not an event
// (a tool's own output, a Python warning) becomes a log event.
const { StringDecoder } = require('node:string_decoder');

const KNOWN = new Set(['step', 'note', 'done', 'log', 'result', 'error']);

function parseLine(line, stream = 'stdout') {
  const text = line.replace(/\r$/, '');
  const t = text.trim();
  if (t.startsWith('{') && t.endsWith('}')) {
    try {
      const o = JSON.parse(t);
      if (o && typeof o === 'object' && typeof o.ev === 'string' && KNOWN.has(o.ev)) return o;
    } catch { /* not JSON: a log line */ }
  }
  return { ev: 'log', text, stream };
}

class LineParser {
  constructor(onEvent, stream = 'stdout') {
    this.onEvent = onEvent;
    this.stream = stream;
    this.decoder = new StringDecoder('utf8');
    this.buf = '';
  }

  push(chunk) {
    this.buf += typeof chunk === 'string' ? chunk : this.decoder.write(chunk);
    let i;
    while ((i = this.buf.indexOf('\n')) >= 0) {
      const line = this.buf.slice(0, i);
      this.buf = this.buf.slice(i + 1);
      this.emit(line);
    }
  }

  end() {
    this.buf += this.decoder.end();
    if (this.buf.length) this.emit(this.buf);
    this.buf = '';
  }

  emit(line) {
    if (!line.replace(/\r$/, '').length) return;   // blank lines carry nothing
    this.onEvent(parseLine(line, this.stream));
  }
}

module.exports = { LineParser, parseLine };
