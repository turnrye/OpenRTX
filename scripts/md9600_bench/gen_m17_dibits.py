"""Pack an M17 stream (oat.m17, openrtx-acceptance-testing) as HR_C6000 TX RAM dibits.

M17 and DMR map symbols to dibits the same way: +3 = 01, +1 = 00, -1 = 10,
-3 = 11, first symbol in bits 7-6. Run from the acceptance repo:
  uv run python gen_m17_dibits.py out.dibits [frames] [src] [dst]
"""
import sys
import numpy as np
from oat.m17 import codec, tools

out = sys.argv[1]
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 50
src = sys.argv[3] if len(sys.argv) > 3 else 'OAT1'
dst = sys.argv[4] if len(sys.argv) > 4 else '@ALL'

payloads = tools.tone_payloads(1000, frames * 0.04)[:frames]
s = np.asarray(codec.stream_symbols(codec.Lsf(dst, src), payloads, 192))
m = {3: 0b01, 1: 0b00, -1: 0b10, -3: 0b11}
d = [m[int(v)] for v in s]
while len(d) % 4:
    d.append(0b01 if len(d) % 2 == 0 else 0b11)
b = bytes((d[i] << 6) | (d[i + 1] << 4) | (d[i + 2] << 2) | d[i + 3]
          for i in range(0, len(d), 4))
open(out, 'wb').write(b)
print(f'{len(s)} symbols, {len(b)} bytes')
