import sys, numpy as np
R = 2_000_000; OFF = 250_000
raw = np.fromfile(sys.argv[1], np.int8).astype(np.float32)
iq = (raw[0::2] + 1j * raw[1::2]) / 128
iq = iq - iq.mean()
iq = iq * np.exp(-2j*np.pi*OFF*np.arange(len(iq))/R)
iq = np.convolve(iq, np.ones(41)/41, "same")
env = np.abs(iq)
win = 2000
p = np.convolve(env**2, np.ones(win)/win, 'same')
on = p > 3 * np.median(p)
idx = np.flatnonzero(on)
if len(idx) == 0:
    print("no burst found; max power %.1f dBFS" % (10*np.log10(p.max()))); sys.exit()
a, b = idx[0], idx[-1]
print("burst %.1f ms long, peak %.1f dBFS, noise %.1f dBFS, clipped %.4f" % ((b-a)/R*1e3, 10*np.log10(p[a:b].mean()), 10*np.log10(np.median(p)), np.mean(np.abs(raw) >= 127)))
seg = iq[a+20000:b-20000]
t = np.arange(len(seg)) / R
pass
# low-pass to ~12 kHz and decimate to 48 kHz
from numpy.fft import rfft
k = 41; h = np.ones(k)/k
bb = seg[::41]
fs = R/41
f = np.angle(bb[1:]*np.conj(bb[:-1])) * fs / (2*np.pi)
print("carrier offset %.0f Hz (mean inst. freq)" % f.mean())
fz = f - f.mean()
print("freq percentiles 1/5/25/50/75/95/99:", np.percentile(fz, [1,5,25,50,75,95,99]).round(0))
h, e = np.histogram(fz, bins=36, range=(-4500, 4500))
for c, lo in zip(h, e[:-1]): print("%6.0f %s" % (lo, '#'*int(50*c/h.max())))
