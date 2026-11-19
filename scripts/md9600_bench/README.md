# MD-9600 HR_C6000 direct-mode bench (test branch only)

Host side of the `CONFIG_I2S_PARROT` console in `platform/targets/MD-9600/i2s_parrot.c`.

- `console_host.py OUTDIR SECONDS`: holds the radio's USB serial port, logs to
  `OUTDIR/serial.log`, turns `PCM ... END` dumps into `takeNNN.wav`, forwards
  lines written to the FIFO `OUTDIR/cmd`.
- `send.py OUTDIR 'cmd' ...`: one command at a time, waits for `DONE` (bursts
  overflow the radio's USB RX ring).
- `gen_m17_dibits.py`: M17 stream to TX RAM dibits for `sym` upload, 128 bytes
  a line (`sym <hex>`).
- `fmcap_look.py`, `burst_look.py`, `check_takes.py`: quick looks at FM copy
  captures, HackRF TX captures and mic takes.
- `rrc_mismatch.py`: RRC 0.2 TX pulse into an M17 RRC 0.5 receiver.

Console commands: `help` on the radio. Results on 2026-11-19, V5, UHF 446.000:

- Mic PCM over I2S3 with the radio's own HR_C6000 set-up, also in physical
  layer TX mode.
- `fmcap`: the FM copy decodes with m17-demod (HackRF source).
- `phytx`: 50/50 frames, EVM 3 %, `dev 78 3e` gives 2402/800 Hz.
- HR_C6000 fast reads need at least 4 NOPs of MISO settle (`rdly`, `rtest`).
