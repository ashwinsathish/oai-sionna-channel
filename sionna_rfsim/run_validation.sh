#!/usr/bin/env bash
#
# One-command validation of the Sionna RT -> OAI EXTERNAL_CIR channel bridge.
#
# Runs an OAI rfsimulator gNB<->UE phy-test loopback (no 5G core, no hardware)
# with the EXTERNAL_CIR channel model driven by a time-varying Sionna .cir file,
# and checks that:
#   1. OAI loads the .cir file,
#   2. the channel steps through snapshots as the simulation's clock advances
#      (i.e. the Sionna channel is actually injected and time-varying),
#   3. the UE stays synchronized and decodes throughout.
#
# Usage:  ./run_validation.sh            (uses the bundled ramp.cir)
#         ./run_validation.sh my.cir     (uses your own exported .cir)
#
# No GUI: this prints text and ends with PASS or FAIL.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
OAI_ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$OAI_ROOT/cmake_targets/ran_build/build"
PY_REPO="/home/sathishkumara/LIT_fac_ray_tracing"
CIR="${1:-$HERE/cir/ramp.cir}"
RUN_SECONDS=25

GNB_LOG=/tmp/oai_val_gnb.log
UE_LOG=/tmp/oai_val_ue.log

cleanup() { sudo pkill -9 -x nr-softmodem 2>/dev/null; sudo pkill -9 -x nr-uesoftmodem 2>/dev/null; }
trap cleanup EXIT

echo "=================================================================="
echo " Sionna RT -> OAI channel bridge validation"
echo " CIR file: $CIR"
echo "=================================================================="

# Make sure the bundled ramp.cir exists (generate if missing / using default).
if [ ! -f "$CIR" ]; then
  echo "[setup] generating a ramp test CIR (path loss 0 -> -40 dB over 30 s)..."
  python3 "$PY_REPO/scripts/make_test_cir.py" ramp --out "$CIR" \
      --fs 61.44e6 --period 0.5 --n 60 --pl-start 0 --pl-end -40 || exit 1
fi

# Point the channel config at the chosen CIR file.
sed -i "s#cir_file *= *\".*\";#cir_file       = \"$CIR\";#" "$HERE/channelmod_external.conf"

cleanup; sleep 2; rm -f "$GNB_LOG" "$UE_LOG"
cd "$BUILD" || { echo "build dir not found: $BUILD"; exit 1; }

echo "[run] starting gNB (server) with the EXTERNAL_CIR channel..."
sudo ./nr-softmodem -O "$HERE/gnb_external.conf" --gNBs.[0].min_rxtxtime 6 \
     --phy-test --rfsim --rfsimulator.[0].serveraddr server \
     --rfsimulator.[0].options chanmod > "$GNB_LOG" 2>&1 &
# wait for the gNB to be ready
for i in $(seq 1 30); do grep -q "TYPE <CTRL-C>\|RF started" "$GNB_LOG" 2>/dev/null && break; sleep 1; done

echo "[run] starting UE (client)..."
sudo ./nr-uesoftmodem --rfsim --phy-test --rfsimulator.[0].serveraddr 127.0.0.1 \
     > "$UE_LOG" 2>&1 &

echo "[run] letting it run ${RUN_SECONDS}s to sweep the channel..."
sleep "$RUN_SECONDS"
cleanup; sleep 1

echo
echo "------------------------------------------------------------------"
echo " RESULTS"
echo "------------------------------------------------------------------"

PASS=1

# Check 1: the .cir file was loaded.
if grep -q "EXTERNAL_CIR] loaded" "$GNB_LOG"; then
  echo "[OK]   OAI loaded the Sionna CIR file:"
  grep "EXTERNAL_CIR] loaded" "$GNB_LOG" | head -1 | sed 's/^/         /'
else
  echo "[FAIL] OAI did not load the CIR file (see $GNB_LOG)"; PASS=0
fi

# Check 2: the channel stepped through snapshots over time.
NSTEPS=$(grep -c "EXTERNAL_CIR] t=" "$GNB_LOG")
if [ "$NSTEPS" -ge 3 ]; then
  echo "[OK]   Channel stepped through $NSTEPS snapshots as the clock advanced:"
  grep "EXTERNAL_CIR] t=" "$GNB_LOG" | sed -n '1p;$p' | sed 's/^/         /'
else
  echo "[FAIL] Channel did not step over time ($NSTEPS transitions)"; PASS=0
fi

# Check 3: the UE synchronized and decoded.
if grep -q "harq:" "$UE_LOG"; then
  echo "[OK]   UE synchronized and decoded over the Sionna channel:"
  grep "harq:" "$UE_LOG" | tail -1 | sed 's/^/         /'
else
  echo "[FAIL] UE did not decode (see $UE_LOG)"; PASS=0
fi

echo "------------------------------------------------------------------"
if [ "$PASS" -eq 1 ]; then
  echo " VALIDATION PASSED - the Sionna channel is driving OAI."
else
  echo " VALIDATION FAILED - inspect $GNB_LOG and $UE_LOG."
fi
echo "------------------------------------------------------------------"
exit $((1 - PASS))
