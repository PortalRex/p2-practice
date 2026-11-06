#!/bin/bash
# Replaces the plugin in a running test copy with the current build: unloads it (by its index in plugin_print),
# copies the new DLL in once the game has let go of the file, and loads it again.
#   tools/testkit/swap-plugin.sh <job> <port>
set -e
job="$1"; port="$2"
root="$(cd "$(dirname "$0")/../.." && pwd)"
netcon="python $root/tools/testkit/netcon.py --port $port"
profile="/c/Program Files (x86)/Steam/steamapps/common/Portal 2/p2practice-test/$job/portal2"
index=$($netcon --wait 1.5 'prac_stop' 'plugin_print' 2>/dev/null | grep "P2 Practice" | head -1 | cut -d: -f1 | tr -d ' \r')
if [ -n "$index" ]; then $netcon --wait 1.5 "plugin_unload $index" >/dev/null 2>&1; fi
copied=no
for attempt in 1 2 3 4 5 6 7 8; do
	if cp "$root/build/x86/plugin/Release/p2practice.dll" "$profile/p2practice.dll" 2>/dev/null; then copied=yes; break; fi
	$netcon --wait 1 'echo waiting for the plugin file' >/dev/null 2>&1
done
[ "$copied" = yes ] || { echo "could not replace the plugin file"; exit 1; }
$netcon --wait 1.5 'plugin_load p2practice' 2>/dev/null | grep "practice\]" | tail -1
