# Eye/ISI of a 4FSK baseband shaped with RRC a_tx, received through RRC a_rx (M17 = 0.5)
import numpy as np
SPS=40; SPAN=16
def rrc(a, sps=SPS, span=SPAN):
    t=np.arange(-span*sps//2, span*sps//2+1)/sps
    h=np.zeros_like(t)
    for i,x in enumerate(t):
        if abs(x)<1e-9: h[i]=1-a+4*a/np.pi
        elif abs(abs(x)-1/(4*a))<1e-9:
            h[i]=a/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*a))+(1-2/np.pi)*np.cos(np.pi/(4*a)))
        else:
            h[i]=(np.sin(np.pi*x*(1-a))+4*a*x*np.cos(np.pi*x*(1+a)))/(np.pi*x*(1-(4*a*x)**2))
    return h/np.sqrt(np.sum(h**2))
rng=np.random.default_rng(1)
N=20000
sym=rng.choice([-3,-1,1,3],N)
up=np.zeros(N*SPS); up[::SPS]=sym
def run(atx,arx):
    y=np.convolve(np.convolve(up,rrc(atx)),rrc(arx))
    d=len(rrc(atx))-1  # total delay = (Ltx-1)/2+(Lrx-1)/2
    best=None
    for off in range(-SPS//2,SPS//2):
        s=y[d+off: d+off+N*SPS: SPS][200:-200]; ref=sym[200:-200]
        g=np.dot(s,ref)/np.dot(ref,ref)
        err=s/g-ref
        rms=np.sqrt(np.mean(err**2)); pk=np.max(np.abs(err))
        if best is None or rms<best[1]: best=(off,rms,pk,g)
    return best
g0=run(0.5,0.5)[3]
for atx in (0.5,0.35,0.2):
    off,rms,pk,g=run(atx,0.5)
    # eye opening between adjacent levels (spacing 2): 2-2*pk ; report in % of ideal and SNR-equivalent of ISI
    print(f"tx a={atx}: best offset {off}/{SPS} sym, gain {g/g0:.3f}, ISI rms {rms/2*100:.1f}% of level spacing, "
          f"worst-case eye {max(0,(2-2*pk)/2)*100:.0f}% open, ISI-limited SNR {20*np.log10(2/2/rms):.1f} dB")
