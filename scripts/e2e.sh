#!/bin/sh
# End-to-end: replay the camera capture over a Gilbert-Elliott channel, run it
# through the receiver, decode the result, and measure it against the intact
# stream.
#
# Three configurations are compared at each loss rate, because the interesting
# question is not "does FEC work" but how much of the remaining damage the
# salvage stage is worth:
#
#   raw          what an ordinary player gets: no recovery, incomplete
#                pictures dropped by the decoder's own rules
#   fec          FlexFEC recovery only
#   fec+salvage  recovery, then a synthesised first slice where one was lost
#
# The channel is parameterised the same way as ../majestic/2026-08-06-ltr/
# fecsim.py (mean loss and mean burst length), so measurements here and
# predictions there are directly comparable.
#
# Needs ffmpeg and ffprobe. Everything else is in this repo.

set -eu

cd "$(dirname "$0")/.."

PCAP=${PCAP:-tests/camera-fec.pcap}
BURST=${BURST:-4}
SEED=${SEED:-1}
RATES=${RATES:-"1 2 5 10"}
OUT=${OUT:-$(mktemp -d)}

command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }
[ -x ./salvage-pcap ] || make salvage-pcap

# The reference: the same capture with nothing dropped. Comparing against the
# camera's original YUV would be better still, but this isolates what the
# receiver did from what the encoder did.
./salvage-pcap --annexb "$OUT/clean.264" "$PCAP" >/dev/null
REF_FRAMES=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of csv=p=0 "$OUT/clean.264")

echo "capture      : $PCAP"
echo "reference    : $REF_FRAMES frames"
echo "channel      : Gilbert-Elliott, mean burst $BURST, seed $SEED"
echo

printf '%-6s %-13s %7s %7s %8s %8s %7s %8s\n' \
    loss config dropped recovrd intact repaired frames psnr_y
printf '%-6s %-13s %7s %7s %8s %8s %7s %8s\n' \
    ---- ------ ------- ------- ------ -------- ------ ------

for rate in $RATES; do
    for cfg in raw fec fec+salvage; do
        case $cfg in
        raw)         flags="--no-fec --no-salvage" ;;
        fec)         flags="--no-salvage" ;;
        fec+salvage) flags="" ;;
        esac

        # shellcheck disable=SC2086
        log=$(./salvage-pcap $flags --loss "$rate" --mean-burst "$BURST" \
            --seed "$SEED" --annexb "$OUT/$cfg-$rate.264" "$PCAP")

        dropped=$(echo "$log" | sed -n 's/^dropped by us *: *//p')
        recovered=$(echo "$log" | sed -n 's/^recovered  *: *//p')
        intact=$(echo "$log" | sed -n 's/^access units.*(\([0-9]*\) intact.*/\1/p')
        repaired=$(echo "$log" | sed -n 's/.*(\([0-9]*\) repaired)/\1/p')
        [ -n "$repaired" ] || repaired=-

        frames=$(ffprobe -v error -count_frames -select_streams v:0 \
            -show_entries stream=nb_read_frames -of csv=p=0 \
            "$OUT/$cfg-$rate.264" 2>/dev/null || echo 0)

        # PSNR only means anything when the two streams line up frame for
        # frame; say so rather than printing a number that compares frame N
        # against frame N+3.
        if [ "$frames" = "$REF_FRAMES" ]; then
            psnr=$(ffmpeg -v info -i "$OUT/$cfg-$rate.264" -i "$OUT/clean.264" \
                -lavfi psnr -f null - 2>&1 |
                sed -n 's/.*PSNR y:\([0-9.]*\).*/\1/p' | tail -1)
        else
            psnr="misaligned"
        fi

        printf '%-6s %-13s %7s %7s %8s %8s %7s %8s\n' \
            "$rate%" "$cfg" "${dropped:-0}" "${recovered:-0}" \
            "${intact:-?}" "$repaired" "$frames" "${psnr:-?}"
    done
    echo
done

echo "streams left in $OUT"
