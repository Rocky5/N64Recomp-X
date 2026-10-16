#!/usr/bin/env python3
"""[AUDDUMP] lines from a serial log (test builds with -DXRC_AUDIO_DUMP=<s>)
-> a 16-bit stereo WAV, plus a few numbers that tell sound from garbage
without listening: RMS, the left/right correlation, and the share of the
energy below 4 kHz (music and effects sit low; data played with the wrong
byte order is white noise, about 4 / (rate / 2) of it low).

usage: auddump_to_wav.py serial.log out.wav

The WAV is the game's own sound: keep it outside the tree."""
import cmath
import math
import struct
import sys
import wave


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rate, pcm = 22050, bytearray()
    for line in open(sys.argv[1], errors='replace'):
        line = line.strip()
        if not line.startswith('[AUDDUMP] '):
            continue
        body = line[10:]
        if body.startswith('rate '):
            rate = int(body.split()[1])
            continue
        for i in range(0, len(body) - 3, 4):
            pcm += struct.pack('<H', int(body[i:i + 4], 16))
    if not pcm:
        sys.exit('no [AUDDUMP] lines (a test build with -DXRC_AUDIO_DUMP=<s>?)')
    with wave.open(sys.argv[2], 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(pcm))
    s = struct.unpack('<%dh' % (len(pcm) // 2), pcm)
    left, right = s[0::2], s[1::2]
    n = len(left)
    rms = math.sqrt(sum(x * x for x in s) / len(s))
    ml, mr = sum(left) / n, sum(right) / n
    cov = sum((a - ml) * (b - mr) for a, b in zip(left, right))
    vl = math.sqrt(sum((a - ml) ** 2 for a in left)) or 1
    vr = math.sqrt(sum((b - mr) ** 2 for b in right)) or 1
    # energy below 4 kHz, from a plain DFT of 1024-sample blocks of the mono mix
    low = tot = 0.0
    blk = 1024
    cut = int(4000 * blk / rate)
    for b0 in range(0, n - blk, blk * 4):
        x = [(left[b0 + i] + right[b0 + i]) / 2 for i in range(blk)]
        for k in range(1, blk // 2):
            c = sum(x[i] * cmath.exp(-2j * math.pi * k * i / blk) for i in range(0, blk, 1))
            e = abs(c) ** 2
            tot += e
            if k < cut:
                low += e
    print('%s: %.2f s at %u Hz, RMS %.0f, L/R correlation %.2f, energy below 4 kHz %.0f %% (white noise: ~%.0f %%)'
          % (sys.argv[2], n / rate, rate, rms, cov / (vl * vr), 100 * low / (tot or 1), 100 * 4000 / (rate / 2)))


if __name__ == '__main__':
    main()
