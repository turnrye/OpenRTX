"""What an ideal FM copy (0-3.4 kHz, 8 kHz, clipped) does to M17, measured the
way copy_measure.py measures the radio's captures.

  uv run python copy_model.py SOURCE.cs8 OUT.wav [band_hz] [clip_counts]
Writes an 8 kHz s16 WAV at the radio's scale (6.9 counts per Hz) for
copy_measure.py.
"""
import sys
import wave
import numpy as np
from scipy.signal import firwin, lfilter, resample_poly
from oat.iq import read_iq, IqFormat

src, out = sys.argv[1], sys.argv[2]
band = float(sys.argv[3]) if len(sys.argv) > 3 else 3400.0
clip = float(sys.argv[4]) if len(sys.argv) > 4 else 32767.0

R = 2_000_000
iq = read_iq(src, IqFormat.CS8)
iq = iq * np.exp(-2j * np.pi * 250000 * np.arange(len(iq)) / R)
iq = resample_poly(iq, 6, 250)                       # 48 kHz
fs = 48000.0
f = np.angle(iq[1:] * np.conj(iq[:-1])) * fs / (2 * np.pi)
lp = firwin(255, band, fs=fs)
f = lfilter(lp, 1.0, f)
f8 = resample_poly(f, 1, 6)                          # 8 kHz
x = np.clip(f8 * 6.9, -clip, clip)
w = wave.open(out, 'wb')
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(8000)
w.writeframes(np.round(x).astype(np.int16).tobytes())
w.close()
print(f'{out}: band {band:.0f} Hz, clip {clip:.0f}, clipped {np.mean(np.abs(x) >= clip):.4f}')
