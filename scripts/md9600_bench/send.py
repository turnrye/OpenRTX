"""Send console commands one at a time, wait for each DONE, print the replies.

usage: send.py OUTDIR 'cmd 1' 'cmd 2' ...
"""
import sys, time

out = sys.argv[1]
logf = out + '/serial.log'
for cmd in sys.argv[2:]:
    start = len(open(logf).read())
    with open(out + '/cmd', 'w') as f:
        f.write(cmd + '\n')
    t = time.time()
    while time.time() - t < 15:
        new = open(logf).read()[start:]
        if '\nDONE' in new or new.startswith('DONE') or ' DONE\n' in new:
            break
        time.sleep(0.1)
    else:
        print(f'!! timeout on {cmd}')
    for line in new.splitlines():
        if ('>> ' in line) or line.endswith(' DONE') or ('> ' + cmd) in line:
            continue
        if line.strip():
            print(line)
