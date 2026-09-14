#!/bin/zsh
# train_unattended.sh <trainer binary>
#
# Runs nnue_policy_train unattended and restarts it after a crash, resuming from the run state the
# trainer writes after every shard (<CKPT_PREFIX>run_state.txt). Configure the run with the usual
# environment variables (CKPT_PREFIX, WEIGHTS, EPOCHS, FEATURE_CACHE, BATCH_SIZE, VALIDATE_N, ...);
# this script adds RESUME=1 once a run state exists.
#
# What it guards against:
#  - The external SSD unmounting. Before every start it waits until the feature cache is readable
#    again, and it logs the USB link speed, because the drive has renegotiated down to USB 2 before.
#  - A crash between or inside shards. The trainer resumes at the last completed shard.
#  - A hang, e.g. a read blocked on a vanished drive. If the log has not grown for STALL_MIN minutes
#    (default 20) the trainer is stopped, by its PID only, and restarted.
#  - A crash that repeats. After 3 failures in a row without the step counter moving, it gives up
#    rather than looping all night on a deterministic error.
#  - Idle sleep. caffeinate keeps the machine awake while the trainer runs.
set -u
BIN=${1:?usage: train_unattended.sh <trainer binary>}
: ${CKPT_PREFIX:?set CKPT_PREFIX}
: ${FEATURE_CACHE:?set FEATURE_CACHE}
STATE=${CKPT_PREFIX}run_state.txt
LOG=${LOG:-${CKPT_PREFIX}train.log}
SUP=${CKPT_PREFIX}supervisor.log
STALL_MIN=${STALL_MIN:-20}
export PYTORCH_MPS_LOW_WATERMARK_RATIO=${PYTORCH_MPS_LOW_WATERMARK_RATIO:-0.6}
export PYTORCH_MPS_HIGH_WATERMARK_RATIO=${PYTORCH_MPS_HIGH_WATERMARK_RATIO:-1.4}
export NUM_WORKERS=1

say() { print -r -- "$(date '+%F %T') $*" | tee -a $SUP; }
state_step() { [[ -f $STATE ]] && sed -n 's/^global_step=//p' $STATE || print -- -1; }

fails=0
last_step=$(state_step)
while true; do
  if [[ -f $STATE ]] && grep -q '^finished=1' $STATE; then
    say "run finished at step $(state_step)"
    exit 0
  fi

  # Readable, not merely present: an unmounted drive leaves the directory missing, and a drive
  # still being checked after an unclean unmount can be listed before it can be read.
  feats=( $FEATURE_CACHE/*.feat(N) )
  until (( ${#feats} > 0 )) && head -c 1048576 ${feats[1]} > /dev/null 2>&1; do
    say "waiting for the feature cache at $FEATURE_CACHE"
    sleep 60
    feats=( $FEATURE_CACHE/*.feat(N) )
  done
  speed=$(system_profiler SPUSBDataType 2>/dev/null | grep -A8 -i "portable ssd" | sed -n 's/^ *Speed: *//p' | head -1)
  say "feature cache readable; SSD link: ${speed:-not reported (not a USB drive?)}"

  if [[ -f $STATE ]]; then resume=1; else resume=0; fi
  say "starting $BIN (RESUME=$resume, step $(state_step))"
  RESUME=$resume $BIN >> $LOG 2>&1 &
  pid=$!
  caffeinate -i -w $pid &
  touch $LOG
  while kill -0 $pid 2> /dev/null; do
    sleep 60
    age=$(( $(date +%s) - $(stat -f %m $LOG) ))
    if (( age > STALL_MIN * 60 )); then
      say "no output for $age s; stopping trainer pid $pid"
      kill $pid 2> /dev/null
      sleep 30
      kill -9 $pid 2> /dev/null
      break
    fi
  done
  wait $pid
  rc=$?

  if (( rc == 0 )); then
    if [[ -f $STATE ]] && grep -q '^finished=1' $STATE; then continue; fi
    say "trainer exited 0 without marking the run finished; not restarting"
    exit 1
  fi
  step=$(state_step)
  if [[ $step == $last_step ]]; then fails=$((fails + 1)); else fails=1; last_step=$step; fi
  say "trainer exited with status $rc at step $step ($fails failure(s) in a row at this step)"
  if (( fails >= 3 )); then
    say "giving up: 3 failures in a row without progress; see $LOG"
    exit 1
  fi
  sleep 120
done
