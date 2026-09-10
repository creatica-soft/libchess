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
# OFFSET starts a pass part-way into the stride, so a later pass is DISJOINT from an earlier one
# instead of overlapping it. Pass 1 at STRIDE=250 OFFSET=0 takes positions 0, 250, 500...; pass 2
# at OFFSET=125 takes 125, 375, 625... and shares nothing with it. Halving the stride instead
# would re-search every position already done -- stride 250 and stride 100 collide on every
# multiple of 500 -- and you would pay full search cost for the duplicates.
#
# Use a separate STATE (and ideally a separate DATASET) per pass:
#   DATASET=targets_p2.tsv STATE=.state_p2 OFFSET=125 ./build_dataset.sh 35000
OFFSET=${OFFSET:-0}
WANT=${1:-20000}

# WHICH ENGINE. gen_targets defaults to /Users/ap/libchess/creatica, and this script used to say
# nothing about it, so a dataset was searched by whatever binary happened to be sitting at that
# path -- which is rebuilt constantly. The 65k records of 2026-09-08 were produced by an engine
# that no longer exists, and nothing in the run recorded what it was. Two changes fix that: the
# engine is named here and passed explicitly, and the run refuses to start if that binary is older
# than the sources it was built from. A silently stale engine is the failure this guards against:
# it produces a perfectly well-formed dataset that simply came from different code.
ENGINE=${ENGINE:-$PWD/creatica}
if [[ ! -x $ENGINE ]]; then
  print "  ERROR: engine $ENGINE not found or not executable"
  print "         set ENGINE=... to choose a different one"
  exit 1
fi
stale=()
for src in creatica_search.cpp creatica_search.hpp creatica.cpp uci_frontend.cpp libchess.h; do
  [[ -f $src && $src -nt $ENGINE ]] && stale+=($src)
done
if (( ${#stale} )); then
  print "  ERROR: $ENGINE:t is older than ${#stale} of its sources: ${stale}"
  print "         rebuild it, or pass ENGINE=/path/to/another to use a specific binary."
  print "         A stale engine still produces a well-formed dataset -- from the wrong code."
  exit 1
fi
export CREATICA_ENGINE=$ENGINE
# Provenance, appended rather than overwritten, so a dataset built over several sessions keeps
# the record of every engine that contributed to it.
engine_id="$(date -u +%Y-%m-%dT%H:%M:%SZ) $ENGINE $(shasum -a 256 $ENGINE | cut -c1-16) mtime=$(date -u -r $ENGINE +%Y-%m-%dT%H:%M:%SZ) movetime=${MOVETIME} hash=${HASH} threads=${THREADS} stride=${STRIDE} offset=${OFFSET} band=${EVAL_LO}..${EVAL_HI}"
print "$engine_id" >> ${DATASET:h}/.dataset_provenance

[[ -f $STATE ]] || : > $STATE

print "  dataset   $DATASET"
print "  engine    $ENGINE  ($(shasum -a 256 $ENGINE | cut -c1-16), built $(date -r $ENGINE '+%Y-%m-%d %H:%M'))"
print "  movetime  ${MOVETIME}ms   stride $STRIDE   eval band ${EVAL_LO}..${EVAL_HI}cp"
print "  target    $WANT records this run"
print ""

done_total=0
for bin in $BINS; do
  (( done_total >= WANT )) && break
  [[ -f $bin ]] || continue
  base=${bin:t:r}
  skip=$(grep "^${base} " $STATE 2>/dev/null | tail -1 | awk '{print $2}')
  skip=${skip:-$OFFSET}
  # A progress file ahead of the recorded state means the previous run was interrupted.
  prog_pre=$STATE.$base.progress
  if [[ -f $prog_pre ]]; then
    p=$(cat $prog_pre 2>/dev/null)
    [[ -n $p ]] && (( p > skip )) && { print "  (resuming from interrupted run at $p)"; skip=$p }
  fi
  remaining=$(( WANT - done_total ))

  print "  === $base (resuming at input $skip, want $remaining more) ==="
  # PROGRESS_FILE is rewritten by gen_targets as it runs, so Ctrl-C does not lose the run.
  # Reading it back is the ONLY reliable resume point: an interrupt kills this script too, so the
  # "resume with SKIP=" line it prints at the end never arrives.
  prog=$STATE.$base.progress
  out=$(./bin2fen $bin 0 $EVAL_LO $EVAL_HI 2>/dev/null \
        | VISIT_DUMP=$DATASET GAME_TAG=$base MOVETIME=$MOVETIME THREADS=$THREADS \
          HASH=$HASH MAX=$remaining SKIP=$skip STRIDE=$STRIDE PROGRESS_FILE=$prog ./gen_targets 2>&1)
  print $out | tail -2

  newskip=$(print $out | grep -o 'SKIP=[0-9]*' | tail -1 | cut -d= -f2)
  [[ -z $newskip && -f $prog ]] && newskip=$(cat $prog)
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
