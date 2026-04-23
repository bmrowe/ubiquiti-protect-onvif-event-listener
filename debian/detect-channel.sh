#!/bin/sh
# detect-channel.sh — detect the user's Protect release channel from
# UniFi OS's runnables.yaml and write a normalised name to
# /etc/onvif-recorder/channel.
#
# runnables.yaml lives at /data/unifi-core/config/runnables.yaml and looks
# like:
#
#   releaseChannels:
#     network: beta
#     protect: beta
#     access: release
#     ...
#
# UniFi OS values (release | release-candidate | beta) map to our apt
# suites (stable | rc | early-access). If the file is missing or
# unparseable we leave the existing /etc/onvif-recorder/channel alone
# (or default to stable on first run).
set -e

CHANNEL_FILE=/etc/onvif-recorder/channel
RUNNABLES=${ONVIF_RUNNABLES_YAML:-/data/unifi-core/config/runnables.yaml}

mkdir -p "$(dirname "$CHANNEL_FILE")"
if [ ! -f "$CHANNEL_FILE" ]; then
    echo "stable" > "$CHANNEL_FILE"
fi

[ -r "$RUNNABLES" ] || exit 0

# Extract `protect:` under the `releaseChannels:` block.  Tolerant of
# whitespace + optional quotes; awk keeps this dependency-free.
RAW=$(awk '
    /^releaseChannels:/ { in_block=1; next }
    in_block && /^[^[:space:]]/ { in_block=0 }
    in_block && /^[[:space:]]+protect[[:space:]]*:/ {
        sub(/^[^:]*:[[:space:]]*/, "")
        gsub(/["'"'"']/, "")
        sub(/[[:space:]]*#.*$/, "")
        print; exit
    }
' "$RUNNABLES" 2>/dev/null | tr -d '[:space:]' | tr '[:upper:]' '[:lower:]')

case "$RAW" in
    release)           NEW=stable ;;
    release-candidate) NEW=rc ;;
    beta|early-access) NEW="early-access" ;;
    *) exit 0 ;;  # unknown / empty — keep previous value
esac

if [ "$(cat "$CHANNEL_FILE" 2>/dev/null)" != "$NEW" ]; then
    echo "$NEW" > "$CHANNEL_FILE"
    logger -t onvif-recorder-channel "Protect channel detected as $NEW (runnables.yaml=$RAW)"
fi

exit 0
