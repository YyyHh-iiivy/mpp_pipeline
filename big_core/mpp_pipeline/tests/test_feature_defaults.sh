#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
header="$root/mpp_pipeline.h"
top_sconscript="$root/../SConscript"
module_sconscript="$root/SConscript"

default_ai=$(sed -n 's/^[[:space:]]*#define[[:space:]]\+AI_BRANCH_ENABLE[[:space:]]\+\([01]\).*/\1/p' "$header" | head -n 1)
default_osd=$(sed -n 's/^[[:space:]]*#define[[:space:]]\+VENC_OSD_ENABLE[[:space:]]\+\([01]\).*/\1/p' "$header" | head -n 1)

[ "$default_ai" = 1 ] || {
    echo "AI_BRANCH_ENABLE must default to 1 for the full-feature build" >&2
    exit 1
}
[ "$default_osd" = 1 ] || {
    echo "VENC_OSD_ENABLE must default to 1 for the full-feature build" >&2
    exit 1
}

grep -q '#ifndef AI_BRANCH_ENABLE' "$header"
grep -q '#ifndef VENC_OSD_ENABLE' "$header"

for sconscript in "$top_sconscript" "$module_sconscript"; do
    grep -q "'AI_BRANCH_ENABLE'" "$sconscript"
    grep -q "'VENC_OSD_ENABLE'" "$sconscript"
    grep -q 'ARGUMENTS.get' "$sconscript"
    grep -q 'CPPDEFINES' "$sconscript"
done

echo "full feature defaults passed"
