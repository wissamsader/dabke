#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 STUDIOMORPHIK
# DABKE's tests in the host simulator, from the repository's root. Build the simulator first:
#   python3 base/tools/build.py app --host
#   ./run_tests.sh [OUTDIR]
set -e
cd "$(dirname "$0")"
H=app/build/host/dabke_host
[ -x "$H" ] || { echo "no simulator: python3 base/tools/build.py app --host"; exit 1; }
OUT="${1:-app/build/host}"
mkdir -p "$OUT"
fail=0
for t in app/tests/*.txt; do
    n=$(basename "$t" .txt)
    if "$H" "$t" "$OUT" > "$OUT/$n.log" 2>&1; then
        echo "ok    $n  ($(grep -c '^ *ok ' "$OUT/$n.log") checks)"
    else
        echo "FAIL  $n  (see $OUT/$n.log)"
        fail=1
    fi
done
exit $fail
