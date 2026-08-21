#!/bin/sh
# hw-e2e.sh — end to end on the real hardware decoder, self-contained.
#
# Replays a captured RTP session (media + FlexFEC repair) through the receiver
# at several Gilbert-Elliott loss rates, in three configurations, and decodes
# each on the platform's hardware decoder. Everything runs inside salvage-play:
# it injects the loss, recovers, salvages, decodes, and reports what the silicon
# produced — frames out, and the frames the decoder flagged (MPP does; a
# GStreamer decoder conceals silently). No external tools.
#
#   raw          --no-fec --no-salvage   no recovery, incomplete pictures
#   fec          --no-salvage            FlexFEC recovery only
#   fec+salvage  (default)               recovery, then first-slice synthesis
#
# Build salvage-play with the backend for this box first (see the README), then:
#   DECODER=mpp ./scripts/hw-e2e.sh capture.pcap        # Rockchip
#   DECODER=gst ./scripts/hw-e2e.sh capture.pcap        # Intel VA-API, etc.

set -u

cd "$(dirname "$0")/.."

PLAY=${PLAY:-./salvage-play}
PCAP=${1:-tests/camera-fec.pcap}
DEC=${DECODER:-auto}
BURST=${BURST:-4}
SEED=${SEED:-1}
RATES=${RATES:-"1 5 10 20"}

[ -x "$PLAY" ] || { echo "build salvage-play first ($PLAY)" >&2; exit 1; }

field() { sed -n "s/^$1 *: *//p"; }

ref=$("$PLAY" --decoder "$DEC" "$PCAP" 2>/dev/null | field "frames decoded")
echo "capture : $PCAP"
echo "decoder : $DEC   reference: ${ref:-?} frames"
echo "channel : Gilbert-Elliott, mean burst $BURST, seed $SEED"
echo

printf '%-6s %-12s %8s %8s %8s\n' loss config dropped frames errinfo
printf '%-6s %-12s %8s %8s %8s\n' ---- ------ ------- ------ -------

for rate in $RATES; do
    for cfg in raw fec salv; do
        case $cfg in
        raw)  flags="--no-fec --no-salvage" ;;
        fec)  flags="--no-salvage" ;;
        salv) flags="" ;;
        esac
        # shellcheck disable=SC2086
        out=$("$PLAY" --decoder "$DEC" $flags --loss "$rate" \
            --mean-burst "$BURST" --seed "$SEED" "$PCAP" 2>/dev/null)
        printf '%-6s %-12s %8s %8s %8s\n' "$rate%" "$cfg" \
            "$(echo "$out" | field 'dropped by us')" \
            "$(echo "$out" | field 'frames decoded')" \
            "$(echo "$out" | field 'decoder errinfo' | grep -oE '[0-9]+' || echo 0)"
    done
    echo
done
