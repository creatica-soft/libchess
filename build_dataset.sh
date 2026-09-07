#!/bin/zsh
# Build policy-distillation targets from the broadcast position bins.
#
#   ./build_dataset.sh [records_this_run]
#
# Each run appends to $DATASET and records how far it got, so it can be stopped with Ctrl-C and
# resumed by running it again. State lives in $STATE, one line per source file.
#
# What the knobs do, and why the defaults are what they are:
#
#   MOVETIME  Depth of the search behind each target, and the whole point of the exercise. A
#             shallow search mostly restates the prior -- measured, agreement with the prior was
#             71% at 500 ms against 64% at 3000 ms, i.e. the shallow search had LESS to teach --
#             and it gets tactical positions wrong: at 500 ms it promoted to a queen where the
#             deep search found an underpromotion. seldepth is recorded per record, so a mixed
#             dataset can be filtered later rather than committed to now.
#
#   STRIDE    bin2fen emits EVERY position of every game, so consecutive inputs differ by one
#             move and their searches largely repeat. A stride spends the same searches on far
#             more distinct material.
#
#   EVAL_LO/HI  Curation band on |Stockfish eval|. A decided position teaches the policy nothing
#             and a dead-drawn one teaches it little; searching them wastes the expensive part.
#             This is the one job Stockfish's evaluation is good for here -- it is deliberately
#             NOT the training target, because creatica already blends policy with eval at search
#             time and training the policy toward eval would shrink the diversity that blend needs.
set -u

BINS=(${BINS:-~/Downloads/lichess_db_broadcast_2020-2025*.bin})
DATASET=${DATASET:-$PWD/targets.tsv}
STATE=${STATE:-$PWD/.dataset_state}
MOVETIME=${MOVETIME:-1000}
STRIDE=${STRIDE:-250}
THREADS=${THREADS:-4}
HASH=${HASH:-2048}
EVAL_LO=${EVAL_LO:-20}
EVAL_HI=${EVAL_HI:-500}
WANT=${1:-20000}

[[ -f $STATE ]] || : > $STATE

print "  dataset   $DATASET"
print "  movetime  ${MOVETIME}ms   stride $STRIDE   eval band ${EVAL_LO}..${EVAL_HI}cp"
print "  target    $WANT records this run"
print ""

done_total=0
for bin in $BINS; do
  (( done_total >= WANT )) && break
  [[ -f $bin ]] || continue
  base=${bin:t:r}
  skip=$(grep "^${base} " $STATE 2>/dev/null | tail -1 | awk '{print $2}')
  skip=${skip:-0}
  remaining=$(( WANT - done_total ))

  print "  === $base (resuming at input $skip, want $remaining more) ==="
  out=$(./bin2fen $bin 0 $EVAL_LO $EVAL_HI 2>/dev/null \
        | VISIT_DUMP=$DATASET GAME_TAG=$base MOVETIME=$MOVETIME THREADS=$THREADS \
          HASH=$HASH MAX=$remaining SKIP=$skip STRIDE=$STRIDE ./gen_targets 2>&1)
  print $out | tail -2

  newskip=$(print $out | grep -o 'SKIP=[0-9]*' | tail -1 | cut -d= -f2)
  got=$(print $out | grep -o 'searched [0-9]*' | tail -1 | awk '{print $2}')
  got=${got:-0}
  if [[ -n $newskip ]]; then
    grep -v "^${base} " $STATE > $STATE.tmp 2>/dev/null || : > $STATE.tmp
    print "$base $newskip" >> $STATE.tmp
    mv $STATE.tmp $STATE
  fi
  done_total=$(( done_total + got ))
  print ""
done

print "  ---"
print "  this run: $done_total records"
if [[ -f $DATASET ]]; then
  print "  dataset:  $(( $(wc -l < $DATASET) - 1 )) records, $(du -h $DATASET | cut -f1)"
fi
print "  analyse:  python3 visit_dump_stats.py $DATASET"
