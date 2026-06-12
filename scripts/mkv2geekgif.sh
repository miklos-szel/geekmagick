#!/usr/bin/env bash
# Convert a video (mp4/mkv/mov/...) into a GeekMagic-ready 240x240 GIF.
# Constraints discovered in the firmware (GeekMagic-Open-Firmware src/display/Gif.cpp):
#   * The draw path clears the previous frame's rect to black whenever a frame
#     uses transparency (the "fuzzy" sparse-pixel screen) -> frames must be
#     partial bounding-box crops WITHOUT transparency (like ezgif output).
# Usage: ./mkv2geekgif.sh input.mp4 [max_bytes] [start-end]
#   max_bytes  size cap (default 1500000, <1.5 MB)
#   start-end  optional excerpt, ffmpeg timestamps, e.g. 0:10-0:30 or 12-40
# Output: input_1.gif, input_2.gif, ... (next free number, never overwrites)
set -euo pipefail

# --- tunables ---
SIZE=240             # square output edge (device screen is 240x240)
FPS=8               # frame rate (NOT auto-lowered: low fps kills the partial-
                     # frame diffing and re-introduces full frames)
COLORS=24            # starting global palette size (dropped if needed to fit)
LOSSY=120            # starting gifsicle lossy strength (raised to fit)
MAXBYTES="${2:-1500000}"   # size cap in bytes (default <1.5 MB)
# ----------------

IN="${1:?usage: $0 input.mp4 [max_bytes] [start-end]}"
[[ -f "$IN" ]] || { echo "no such file: $IN" >&2; exit 1; }

# Optional excerpt: 3rd arg "START-END" -> ffmpeg input seek (timestamps may be
# seconds or H:MM:SS / M:SS; they never contain '-', so split on it).
CLIP="${3:-}"
SEEK=()
if [[ -n "$CLIP" ]]; then
  start="${CLIP%-*}"; end="${CLIP#*-}"
  [[ "$start" != "$CLIP" && -n "$start" && -n "$end" ]] \
    || { echo "bad clip range: '$CLIP' (use START-END, e.g. 0:10-0:30)" >&2; exit 1; }
  SEEK=(-ss "$start" -to "$end")
fi

base="${IN%.*}"
n=1
while [[ -e "${base}_${n}.gif" ]]; do ((n++)); done
OUT="${base}_${n}.gif"
RAW="$(mktemp -t mkv2geekgif.XXXXXX).gif"
trap 'rm -f "$RAW"' EXIT

# Re-encode at a given fps + palette size: optional excerpt, force-scale to
# square, single global palette, NO dithering. ffmpeg's offsetting (on by
# default) emits partial bounding-box frames; `-gifflags -transdiff` disables
# transparency diffing, because the firmware's transparency draw path erases
# the previous frame's rect (fuzzy screen).
encode_raw() {  # $1=fps  $2=colors
  ffmpeg -y -v error ${SEEK[@]+"${SEEK[@]}"} -i "$IN" -vf \
    "scale=${SIZE}:${SIZE}:flags=lanczos,fps=${1},split[a][b];\
[a]palettegen=max_colors=${2}:stats_mode=full[p];\
[b][p]paletteuse=dither=none" \
    -gifflags -transdiff "$RAW"
}

# Optimize + shrink to fit MAXBYTES. gifsicle -O1 re-crops each frame to its
# changed bounding box WITHOUT introducing transparency (-O2/-O3 add transparent
# pixels, which this firmware renders by erasing the previous frame -> fuzz).
# To stay under the cap we raise --lossy then drop the palette; both keep the
# partial-frame, no-transparency structure. fps is never lowered (low fps makes
# frames stop overlapping and the crops balloon to full canvas). EFF_* hold the
# settings that produced OUT.
fit() {
  EFF_COLORS="$COLORS"; EFF_LOSSY="$LOSSY"
  encode_raw "$FPS" "$EFF_COLORS"
  while :; do
    gifsicle -O1 --lossy=${EFF_LOSSY} --loopcount=forever "$RAW" -o "$OUT"
    local size; size="$(stat -f%z "$OUT")"
    if [[ "$size" -le "$MAXBYTES" ]]; then return 0; fi
    if   [[ "$EFF_LOSSY"  -lt 300 ]]; then
      EFF_LOSSY=$((EFF_LOSSY + 40))
    elif [[ "$EFF_COLORS" -gt 8 ]]; then
      EFF_COLORS=$((EFF_COLORS > 16 ? 16 : EFF_COLORS - 4))
      encode_raw "$FPS" "$EFF_COLORS"
    else
      echo "warn: ${size} bytes > ${MAXBYTES} cap at the quality floor (${EFF_COLORS}col)." >&2
      echo "      Shorten the clip, e.g.: $0 \"$IN\" $MAXBYTES 0:00-0:10" >&2
      return 0
    fi
  done
}
fit

# Firmware-compat guard (GeekMagic-Open-Firmware src/display/Gif.cpp):
#   - no transparent frames (transparency draw path erases previous frame)
#   - no disposal "background"/"previous" (also triggers the erase path)
info="$(gifsicle --info "$OUT" 2>/dev/null)"
transp="$(grep -c 'transparent' <<<"$info" || true)"
baddisp="$(grep -Ec 'disposal (background|previous)' <<<"$info" || true)"
frames="$(grep -c '+ image' <<<"$info" || true)"
[[ "$transp"  -gt 0 ]] && echo "warn: $transp transparent frames -- screen will go fuzzy on device" >&2
[[ "$baddisp" -gt 0 ]] && echo "warn: $baddisp frames use background/previous disposal -- may erase image" >&2

printf '%s  (%s bytes, %s, %s frames @ %sfps/%scol%s, ~%ss, cap %s)\n' \
  "$OUT" "$(stat -f%z "$OUT")" \
  "$(identify -format '%Wx%H' "${OUT}[0]" 2>/dev/null || echo '?')" \
  "$frames" "$FPS" "$EFF_COLORS" "${CLIP:+, clip $CLIP}" "$((frames / FPS))" "$MAXBYTES"
