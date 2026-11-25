#!/bin/sh
# SPDX-License-Identifier: MIT
# EPICYCLE firmware 1.4 top-of-card bridge.
#
# On 1.4.x with Developer Mode enabled, stock ScriptRunner looks for autorun.sh
# at the root of each drive, asks once per boot, and on Yes runs it via
# systemd-run. This file only chains into autorun/autorun.sh (the same entry
# firmware 1.3 runs after verifying autorun.check).
#
# Do not flash firmware. SD autorun only.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec /bin/sh "$HERE/autorun/autorun.sh"
