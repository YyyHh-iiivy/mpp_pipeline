#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MAKEFILE="$ROOT/Makefile"
GITIGNORE="$ROOT/../.gitignore"

require_pattern()
{
    pattern=$1
    message=$2

    if ! grep -Eq "$pattern" "$MAKEFILE"; then
        echo "$message" >&2
        exit 1
    fi
}

reject_pattern()
{
    pattern=$1
    message=$2

    if grep -Eq "$pattern" "$MAKEFILE"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern '^TARGET[[:space:]]*:=[[:space:]]*user/rtsp_sender_withsd_compactdiag$' \
    'the workspace build target must remain rtsp_sender_withsd_compactdiag'
require_pattern 'mkdir[[:space:]]+-p[[:space:]]+\$\(dir[[:space:]]+\$\(TARGET\)\)' \
    'make build must create the versioned user output directory'
reject_pattern 'test[[:space:]]+-x[[:space:]]+\$\(PRESERVED_' \
    'make verify must not depend on excluded historical ELF files'

if grep -Eq '^[[:space:]]*small_core/user(/|[[:space:]]|$)' "$GITIGNORE"; then
    echo 'small_core/user must remain available for GitHub executable downloads' >&2
    exit 1
fi

echo 'small-core workspace build rules passed'
