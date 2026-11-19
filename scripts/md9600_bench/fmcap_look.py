import sys, wave, numpy as np
w = wave.open(sys.argv[1]); x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float)
print("n", len(x), "mean %.0f std %.0f" % (x.mean(), x.std()))
print("clipped frac %.4f" % np.mean(np.abs(x) >= 32767))
print("percentiles 1/5/25/50/75/95/99:", np.percentile(x, [1,5,25,50,75,95,99]).round(0))
h, e = np.histogram(x, bins=40)
for c, lo in zip(h, e[:-1]): print("%7.0f %s" % (lo, '#' * int(60 * c / h.max())))
X = np.abs(np.fft.rfft(x - x.mean())); f = np.fft.rfftfreq(len(x), 1/8000)
for lo, hi in [(0,300),(300,1200),(1200,2400),(2400,3400),(3400,4000)]:
    print(f"{lo}-{hi} Hz energy {np.sum(X[(f>=lo)&(f<hi)]**2)/np.sum(X**2):.2f}")
