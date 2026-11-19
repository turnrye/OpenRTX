"""Measure M17 in HR_C6000 FM copy captures with the bench decoder (oat.m17).

The copy is the chip's FM discriminator at 8 kHz. Rebuild FM IQ from it (phase
= integral of frequency) at 48 kHz and hand that to measure_m17_capture.
Run from the acceptance repo:
  uv run python copy_measure.py COUNTS_PER_HZ take000.wav [take001.wav ...]
"""
import sys
import wave
import numpy as np
from scipy.signal import resample_poly
from oat.m17 import phy

scale = float(sys.argv[1])
for path in sys.argv[2:]:
    w = wave.open(path)
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float)
    hz = resample_poly(x - np.median(x), 6, 1) / scale       # 48 kHz, Hz
    rate = 48000.0
    iq = np.exp(1j * 2 * np.pi * np.cumsum(hz) / rate)
    pad = np.zeros(int(0.3 * rate), complex) + 1e-4
    iq = np.concatenate([pad, iq, pad])
    ms = phy.measure_m17_capture(iq, rate, 0.0)
    name = path.split('/')[-1]
    if not ms:
        print(f'{name}: no transmission found')
    for m in ms:
        print(f'{name}: {m.duration_s:.2f} s, LSF {m.lsf_found}/{m.lsf_crc_ok}, '
              f'{m.source}->{m.destination}, frames {m.stream_frames}, '
              f'FN errors {m.frame_number_errors}, EOT {m.eot_found}, '
              f'outer {m.outer_deviation_hz:.0f} Hz ratio {m.deviation_ratio:.2f}, '
              f'EVM {m.evm_percent:.1f} %, polarity {m.polarity}')
