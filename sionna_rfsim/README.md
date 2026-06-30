# Sionna RT channel in OAI (EXTERNAL_CIR)

This folder drives an OAI RFsimulator 5G link with the **site-specific channel
computed by our Sionna RT digital twin** instead of OAI's built-in statistical
models (AWGN / Rician / TDL). It needs **no radio hardware and no 5G core** —
the gNB and UE run as two programs on one machine and talk over a TCP socket.

OAI has **no GUI**: everything is command-line, and "results" are numbers in
the text logs (SNR, BLER, throughput) plus channel-stepping messages.

## Quick test (one command)

```bash
cd ~/openairinterface5g/sionna_rfsim
./run_validation.sh
```
This starts the gNB + UE with a time-varying test channel, runs ~25 s, and
prints **VALIDATION PASSED** if OAI loaded the Sionna channel file, stepped
through it as the clock advanced, and the UE decoded. (Uses `sudo` for OAI's
real-time scheduling.)

To run it with a real channel exported from Sionna:
```bash
./run_validation.sh /path/to/your_export/ap0_to_ue.cir
```

## What the result means

- **"OAI loaded the Sionna CIR file"** — OAI is reading the exported Sionna channel,
  not a textbook model.
- **"Channel stepped through N snapshots as the clock advanced"** — the channel
  changes over time exactly as the AGV moves through the factory (each line
  shows the simulation time, the snapshot index, and the path loss at that
  moment).
- **"UE synchronized and decoded"** — a real 5G link ran over that channel.

## How to produce a Sionna channel file

In the LIT_fac_ray_tracing repo (the Sionna side):
```bash
python scripts/export_oai_cir.py \
    --scene LIT_factory_with_rad/Factory_with_radiators.xml \
    --csv  logs/live_sim_runs/<a recorded AGV run>/csv_<run>.csv \
    --fs   61.44e6 \
    --out  cir_out/
```
This ray-traces the AGV trajectory and writes one `.cir` file per access
point. Point `run_validation.sh` (or `channelmod_external.conf`) at one of
them. **`--fs` must match the OAI run's sample rate** (61.44 Msps for the
106-PRB / 30 kHz config used here).

## Run it manually (two terminals)

Terminal 1 (gNB):
```bash
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo ./nr-softmodem -O ../../../sionna_rfsim/gnb_external.conf \
     --gNBs.[0].min_rxtxtime 6 --phy-test --rfsim \
     --rfsimulator.[0].serveraddr server --rfsimulator.[0].options chanmod
```
Terminal 2 (UE):
```bash
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo ./nr-uesoftmodem --rfsim --phy-test --rfsimulator.[0].serveraddr 127.0.0.1
```
Watch the gNB terminal for `[EXTERNAL_CIR] t=... snapshot .../...` lines (the
channel stepping) and the per-UE `dlsch_rounds/ulsch_rounds ... BLER ...` KPI
lines. Ctrl-C both to stop.

## Files here

| File | Purpose |
|---|---|
| `run_validation.sh` | one-command end-to-end test |
| `gnb_external.conf` | gNB phy-test config (SISO) that `@include`s the channel |
| `channelmod_external.conf` | selects `type = "EXTERNAL_CIR"` and the `.cir` file |
| `channelmod_awgn.conf` | the AWGN baseline (for comparison / sanity) |
| `cir/` | test `.cir` files (`unit_tap.cir`, `ramp.cir`) |

## To switch back to a normal OAI channel

Edit `gnb_external.conf`'s `@include` to `channelmod_awgn.conf` (AWGN), or set
`type` to any built-in model. The EXTERNAL_CIR model is an **added option** —
all of OAI's normal channels and features are unchanged.

Full design + implementation notes:
`LIT_fac_ray_tracing/docs/OAI_SIONNA_CHANNEL_INTEGRATION.md`.
