#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'Python 3.9 or later is required. Install it using your distribution package manager.' >&2
    exit 2
fi
exec python3 -B "$script_dir/Update-YamahaHybrids.py" "$@"
