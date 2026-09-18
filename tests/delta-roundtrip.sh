#!/bin/sh
# Round-trip test for `warp delta` / `warp delta-apply`:
#  * a modified copy of a random file must be rebuilt bit-for-bit,
#  * the delta must be far smaller than the new file,
#  * a delta applied to the wrong old file must be refused,
#  * a truncated delta must be refused.
set -eu
WARP="${WARP:-$(dirname "$0")/../warp}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

head -c 6000000 /dev/urandom > "$T/old"
# new = 4 KiB inserted at the front + a changed 8 KiB block in the middle + tail appended
{
    head -c 4096 /dev/urandom
    head -c 3000000 "$T/old"
    head -c 8192 /dev/urandom
    tail -c +3008193 "$T/old"
    head -c 100000 /dev/urandom
} > "$T/new"

"$WARP" delta "$T/old" "$T/new" "$T/d" > "$T/delta.log"
"$WARP" delta-apply "$T/old" "$T/d" "$T/rebuilt" > /dev/null
cmp "$T/new" "$T/rebuilt"

new_size=$(stat -c %s "$T/new")
delta_size=$(stat -c %s "$T/d")
# 112 KiB of genuinely new bytes plus chunk slop; anything under 5% is healthy.
limit=$((new_size / 20))
if [ "$delta_size" -gt "$limit" ]; then
    echo "delta too large: $delta_size bytes for a $new_size byte file" >&2
    cat "$T/delta.log" >&2
    exit 1
fi
echo "roundtrip ok: new=$new_size delta=$delta_size"

# Wrong base must be rejected.
head -c 6000000 /dev/urandom > "$T/other"
if "$WARP" delta-apply "$T/other" "$T/d" "$T/bad" > /dev/null 2>&1; then
    echo "delta applied to the wrong old file" >&2; exit 1
fi
[ ! -f "$T/bad" ]
echo "wrong base rejected"

# Truncated delta must be rejected.
head -c $((delta_size - 100)) "$T/d" > "$T/trunc"
if "$WARP" delta-apply "$T/old" "$T/trunc" "$T/bad2" > /dev/null 2>&1; then
    echo "truncated delta accepted" >&2; exit 1
fi
[ ! -f "$T/bad2" ]
echo "truncated delta rejected"

# Identical files: delta is tiny.
"$WARP" delta "$T/old" "$T/old" "$T/same" > /dev/null
[ "$(stat -c %s "$T/same")" -lt 4096 ]
echo "identical files -> $(stat -c %s "$T/same") byte delta"
echo "PASS"
