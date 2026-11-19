"""Hold the MD-9600 parrot console: serial -> log (+ WAV from PCM dumps), FIFO -> serial.

usage: console_host.py OUTDIR SECONDS
Send a command:  echo 'rec 3000' > OUTDIR/cmd
Read replies:    OUTDIR/serial.log
"""
import base64, glob, os, select, sys, time, wave
import serial

out, dur = sys.argv[1], float(sys.argv[2])
os.makedirs(out, exist_ok=True)
fifo = os.path.join(out, 'cmd')
if not os.path.exists(fifo):
    os.mkfifo(fifo)
log = open(os.path.join(out, 'serial.log'), 'a', buffering=1)
fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
end = time.time() + dur
pcm = None
pending = b''


def handle(line):
    global pcm
    if line.startswith('PCM '):
        _, take, n, rate, _fmt = line.split()
        pcm = (int(take), int(rate), [])
        log.write(f'{time.strftime("%T")} {line}\n')
    elif line.startswith('END ') and pcm:
        take, rate, parts = pcm
        data = base64.b64decode(''.join(parts))
        fn = os.path.join(out, f'take{take:03d}.wav')
        with wave.open(fn, 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
            w.writeframes(data)
        log.write(f'{time.strftime("%T")} {line} -> {fn}\n')
        pcm = None
    elif pcm:
        pcm[2].append(line)
    elif line:
        log.write(f'{time.strftime("%T")} {line}\n')


while time.time() < end:
    ports = glob.glob('/dev/cu.usbmodem*')
    if not ports:
        time.sleep(0.3); continue
    try:
        s = serial.Serial(ports[0], 115200, timeout=0)
        s.dtr = True
        log.write(f'== open {ports[0]} {time.strftime("%T")}\n')
        buf = b''
        while time.time() < end:
            r, _, _ = select.select([s.fileno(), fd], [], [], 0.2)
            if fd in r:
                data = os.read(fd, 4096)
                if data:
                    pending += data
                else:  # writer closed: reopen so the next echo is seen
                    os.close(fd)
                    fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
            if pending:
                s.write(pending.replace(b'\n', b'\r\n'))
                log.write(f'{time.strftime("%T")} >> {pending.decode().strip()}\n')
                pending = b''
            if s.fileno() in r:
                buf += s.read(8192)
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    handle(raw.decode('ascii', 'replace').strip())
    except (serial.SerialException, OSError) as e:
        log.write(f'== closed {time.strftime("%T")} {e}\n'); pcm = None
        time.sleep(0.3)
log.write('== host done\n')
