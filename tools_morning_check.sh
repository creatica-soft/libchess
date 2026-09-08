#!/bin/zsh
# tools_morning_check.sh - read an overnight bot run for the failure modes that have actually
# happened in this project, rather than just the score.
#
#   ./tools_morning_check.sh [since]        since = "2026-09-08" or "2026-09-08T20" (UTC)
#
# Read-only. Every check below corresponds to something that really went wrong at least once,
# and each one is silent in the results table -- a crashed engine, an illegal move, a stall and
# a lost-on-time game all look like an ordinary loss from the score alone.

cd /Users/ap/libchess || exit 1
SINCE="${1:-$(date -u -v-1d +%Y-%m-%d)}"

echo "=== overnight check, games since ${SINCE} (UTC) ==="
echo

echo "-- crashes (the engine dying looks like a stall to the driver, not like a bug)"
n=$(ls -1 ~/Library/Logs/DiagnosticReports/creatica*.ips 2>/dev/null | wc -l | tr -d ' ')
recent=$(find ~/Library/Logs/DiagnosticReports -name 'creatica*.ips' -newermt "${SINCE}" 2>/dev/null | wc -l | tr -d ' ')
echo "   crash reports total ${n}, since ${SINCE}: ${recent}"
find ~/Library/Logs/DiagnosticReports -name 'creatica*.ips' -newermt "${SINCE}" 2>/dev/null | sed 's/^/     /'
echo

for f in creatica_creaticachessbot.log creatica_creaticachessbot2.log; do
  [ -f "$f" ] || continue
  echo "-- ${f}"
  printf "   illegal moves refused      %s   (castling-notation or replay divergence)\n" "$(grep -c 'refusing illegal move' $f)"
  printf "   tree validator violations  %s   (only meaningful with ValidateTree on)\n"  "$(grep -c 'violation(s)' $f)"
  printf "   accumulator limit hits     %s   (unbounded recursion somewhere)\n"          "$(grep -c 'reached the .* limit' $f)"
  printf "   unevaluated-child warnings %s   (search reaching a node it cannot score)\n" "$(grep -c 'unevaluated child' $f)"
  printf "   leaf-node-in-check         %s   (process_check() should make this impossible)\n" "$(grep -c 'leaf node in check' $f)"
  printf "   repetition skips           %s   (winning position steered away from a draw)\n"   "$(grep -c 'skipping move' $f)"
  echo "   slowest collections:"
  grep -oE "gc took [0-9.]+ ms" $f | awk '{print $3}' | sort -g | tail -3 | sed 's/^/     /'
  echo
done

echo "-- driver-level failures (these appear on the bot's stdout, not in the engine log)"
echo "   grep your terminal scrollback for: 'produced nothing', 'returned false', 'restarting', 'HTTP code: 4'"
echo

echo "-- results"
for r in results_creaticachessbot.csv results_creaticachessbot2.csv; do
  [ -f "$r" ] || continue
  awk -F, -v s="$SINCE" 'NR>1 && $1>=s {n++; pts+=$6; st[$7]++}
    END{ if(n) { printf "   %-34s %3d games, %5.1f pts (%.1f%%)\n", FILENAME, n, pts, 100*pts/n;
                 for(k in st) printf "        %-14s %d\n", k, st[k] } }' "$r"
done
echo
echo "   NOTE: 'outoftime' is the one to look at first. Time management was NOT changed in"
echo "   either binary of this match, so a flag here is a real regression, not the new allocator."
