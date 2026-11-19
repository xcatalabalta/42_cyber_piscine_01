#!/usr/bin/env bash
#
# spider - recursively download images from a website (pure shell logic).
#
# Usage:   ./spider [-r] [-l N] [-p PATH] URL
#   -r         follow links recursively
#   -l N       max recursion depth (default 5, needs -r)
#   -p PATH    output directory (default ./data/)
#
# curl is used ONLY to fetch a single URL (HTTP request + file handling,
# which the subject allows). The spider logic -- HTML parsing, URL
# resolution, recursion, dedup -- is written here. wget is NOT used,
# because `wget -r` would be the spider itself.

set -u

EXT_REGEX='\.(jpe?g|png|gif|bmp)$'
UA="arachnida-spider/1.0"

recursive=0
max_depth=5
out="./data"

usage() { echo "Usage: $0 [-r] [-l N] [-p PATH] URL" >&2; exit 1; }

# --- argument parsing -------------------------------------------------------
while getopts ":rl:p:" opt; do
    case "$opt" in
        r) recursive=1 ;;
        l) max_depth="$OPTARG" ;;
        p) out="$OPTARG" ;;
        \?) echo "Unknown option -$OPTARG" >&2; usage ;;
        :)  echo "Option -$OPTARG needs an argument" >&2; usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
start_url="$1"

case "$start_url" in
    http://*|https://*) ;;
    *) echo "URL must start with http:// or https://" >&2; exit 1 ;;
esac
case "$max_depth" in
    ''|*[!0-9]*) echo "-l must be a non-negative integer" >&2; exit 1 ;;
esac

mkdir -p "$out" || exit 1

# host of the start URL, to stay on the same domain while recursing
start_host="$(printf '%s' "$start_url" | sed -E 's#^[a-z]+://([^/]+).*#\1#')"

# state files (associative arrays would need bash 4; temp files are portable)
visited_pages="$(mktemp)"
downloaded="$(mktemp)"
trap 'rm -f "$visited_pages" "$downloaded"' EXIT

img_count=0

# --- collapse /./ and /seg/../ segments in a URL so dedup is reliable -------
normalize_url() {
    local u="$1" scheme rest host path query=""
    scheme="${u%%://*}"
    rest="${u#*://}"
    host="${rest%%/*}"
    if [ "$rest" = "$host" ]; then printf '%s://%s' "$scheme" "$host"; return; fi
    path="/${rest#*/}"
    case "$path" in *\?*) query="?${path#*\?}"; path="${path%%\?*}" ;; esac

    local out="" seg
    local oldIFS="$IFS"; IFS=/
    for seg in $path; do
        case "$seg" in
            ''|.) ;;                                   # drop empty and "."
            ..)  out="${out%/*}" ;;                    # pop last segment
            *)   out="$out/$seg" ;;
        esac
    done
    IFS="$oldIFS"
    printf '%s://%s%s%s' "$scheme" "$host" "${out:-/}" "$query"
}

# --- URL resolution: join a (possibly relative) ref against a base URL ------
resolve_url() {
    local base="$1" ref="$2" joined
    ref="${ref%%#*}"                       # drop fragment
    case "$ref" in
        http://*|https://*) normalize_url "$ref"; return ;;
        //*) normalize_url "${base%%:*}:$ref"; return ;;       # protocol-relative
    esac
    local scheme="${base%%://*}"
    local rest="${base#*://}"
    local host="${rest%%/*}"
    case "$ref" in
        /*) joined="$scheme://$host$ref" ;;                    # root-relative
        *)  joined="${base%%\?*}"; joined="${joined%/*}/$ref" ;;# path-relative
    esac
    normalize_url "$joined"
}

basename_from_url() {
    local u="${1%%\?*}"; u="${u%%#*}"; printf '%s' "${u##*/}"
}

seen() { grep -qxF "$2" "$1"; }   # seen <file> <line>

download_image() {
    local url="$1"
    seen "$downloaded" "$url" && return
    printf '%s\n' "$url" >> "$downloaded"

    local name; name="$(basename_from_url "$url")"
    [ -z "$name" ] && name="image"
    local dest="$out/$name"
    if [ -e "$dest" ]; then                 # avoid clobber on basename clash
        local root="${name%.*}" ext="${name##*.}" i=1
        while [ -e "$out/${root}_${i}.${ext}" ]; do i=$((i+1)); done
        dest="$out/${root}_${i}.${ext}"
    fi

    if curl -fsSL -A "$UA" --max-time 20 -o "$dest" "$url"; then
        img_count=$((img_count+1))
        echo "  [img ] $url -> $dest"
    else
        echo "  [skip] $url" >&2
        rm -f "$dest"
    fi
}

# extract attribute values (src= / href= / srcset=) from stdin HTML.
# NOTE: stdin can only be read once, so buffer it before the two passes.
extract_refs() {
    local html; html="$(cat)"
    # quoted values of src= and href=
    printf '%s' "$html" \
        | grep -oiE '(src|href)[[:space:]]*=[[:space:]]*("[^"]*"|'\''[^'\'']*'\'')' \
        | sed -E 's/^[^=]*=[[:space:]]*//; s/^.//; s/.$//'
    # srcset: take the first token (the URL) of each comma-separated candidate
    printf '%s' "$html" \
        | grep -oiE 'srcset[[:space:]]*=[[:space:]]*("[^"]*"|'\''[^'\'']*'\'')' \
        | sed -E 's/^[^=]*=[[:space:]]*//; s/^.//; s/.$//' \
        | tr ',' '\n' | sed -E 's/^[[:space:]]*//; s/[[:space:]].*$//'
}

crawl() {
    local url="$1" depth="$2"
    url="${url%%#*}"
    seen "$visited_pages" "$url" && return
    printf '%s\n' "$url" >> "$visited_pages"
    echo "[page] depth $depth: $url"

    local html; html="$(curl -fsSL -A "$UA" --max-time 20 "$url" 2>/dev/null)" || {
        echo "  [skip] $url" >&2; return; }

    local ref abs
    while IFS= read -r ref; do
        [ -z "$ref" ] && continue
        abs="$(resolve_url "$url" "$ref")"
        if printf '%s' "${abs%%\?*}" | grep -qiE "$EXT_REGEX"; then
            download_image "$abs"
        elif [ "$recursive" -eq 1 ] && [ "$depth" -lt "$max_depth" ]; then
            local h; h="$(printf '%s' "$abs" | sed -E 's#^[a-z]+://([^/]+).*#\1#')"
            [ "$h" = "$start_host" ] && crawl "$abs" $((depth+1))
        fi
    done < <(printf '%s' "$html" | extract_refs)
}

crawl "$start_url" 0
echo
echo "Done. $img_count image(s) saved to $(cd "$out" && pwd)"