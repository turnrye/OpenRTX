import sys, wave, numpy as np
files = sys.argv[2:]; out = sys.argv[1]; allx = []
for f in files:
    w = wave.open(f); x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float); allx.append(x)
    X = np.abs(np.fft.rfft(x)); fr = np.fft.rfftfreq(len(x), 1/8000)
    band = X[(fr > 300) & (fr < 3400)].sum() / X[fr > 20].sum()
    env = np.array([np.sqrt(np.mean(x[i:i+160]**2)) for i in range(0, len(x)-160, 160)])
    E = np.abs(np.fft.rfft(env - env.mean())); ef = np.fft.rfftfreq(len(env), 0.02)
    syl = E[(ef > 2) & (ef < 8)].sum() / E[ef > 0.3].sum()
    print(f"{f.split('/')[-1]}: rms {x.std():.0f} voiceband {band:.2f} envelope 2-8 Hz share {syl:.2f} loud/quiet frames {np.percentile(env,90)/max(np.percentile(env,10),1):.1f}x")
y = np.concatenate(allx); y = y / max(1, np.abs(y).max()) * 20000
w = wave.open(out, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000); w.writeframes(y.astype(np.int16).tobytes()); w.close()
print("wrote", out)
