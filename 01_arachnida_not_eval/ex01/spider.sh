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
TAB="$(printf '\t')"

# resources we must never recurse INTO (they aren't HTML pages)
ASSET_REGEX='\.(js|mjs|css|map|json|xml|rss|mp4|webm|mov|avi|mp3|wav|m4a|svg|pdf|zip|gz|woff2?|ttf|eot|ico|webp|txt)(\?|$)'

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
    # host-only URL -> give it the canonical "/" path so "scheme://host" and
    # "scheme://host/" collapse to one form (matches the page's canonical link)
    if [ "$rest" = "$host" ]; then printf '%s://%s/' "$scheme" "$host"; return; fi
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

# reject refs that aren't real, fetchable URLs (JS snippets, data:/mailto:, #frag)
valid_ref() {
    local r="$1"
    [ -z "$r" ] && return 1
    [ "${#r}" -gt 1024 ] && return 1
    printf '%s' "$r" | grep -qE '^[A-Za-z0-9._~:/?#@!$&*+,;=%-]+$' || return 1
    printf '%s' "$r" | grep -qiE '^(javascript:|data:|mailto:|tel:|#)' && return 1
    return 0
}

# true if the URL points at a non-HTML asset -> never recurse into it
is_asset_ext() { printf '%s' "${1%%\#*}" | grep -qiE "$ASSET_REGEX"; }

# strip <script>/<style> block CONTENTS so inline JS isn't mined for fake links
strip_blocks() {
    if command -v perl >/dev/null 2>&1; then
        perl -0777 -pe 's{<script\b.*?</script>}{}gis; s{<style\b.*?</style>}{}gis'
    else
        cat   # graceful fallback: valid_ref still rejects most JS garbage
    fi
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

# Extract candidate refs from stdin HTML, each tagged with its kind:
#   L<TAB>url  -> from an href: may drive recursion (also downloaded if an image)
#   I<TAB>url  -> from src / srcset: image candidate only, never recursion
# Inline <script>/<style> are stripped first so their JS can't be mined.
extract_refs() {
    local html; html="$(cat | strip_blocks)"
    # href= values -> recursable (anchors, and <link> favicons that are images)
    printf '%s' "$html" \
        | grep -oiE 'href[[:space:]]*=[[:space:]]*("[^"]*"|'\''[^'\'']*'\'')' \
        | sed -E "s/^[^=]*=[[:space:]]*//; s/^.//; s/.\$//; s/^/L${TAB}/"
    # src= values -> image only
    printf '%s' "$html" \
        | grep -oiE 'src[[:space:]]*=[[:space:]]*("[^"]*"|'\''[^'\'']*'\'')' \
        | sed -E "s/^[^=]*=[[:space:]]*//; s/^.//; s/.\$//; s/^/I${TAB}/"
    # srcset= -> first token (URL) of each comma-separated candidate, image only
    printf '%s' "$html" \
        | grep -oiE 'srcset[[:space:]]*=[[:space:]]*("[^"]*"|'\''[^'\'']*'\'')' \
        | sed -E 's/^[^=]*=[[:space:]]*//; s/^.//; s/.$//' \
        | tr ',' '\n' | sed -E "s/^[[:space:]]*//; s/[[:space:]].*\$//; s/^/I${TAB}/"
}

crawl() {
    local url="$1" depth="$2"
    url="${url%%#*}"
    seen "$visited_pages" "$url" && return
    printf '%s\n' "$url" >> "$visited_pages"
    echo "[page] depth $depth: $url"

    # fetch to a temp file (never pull possibly-binary bodies into a shell var)
    local body; body="$(mktemp)"
    if ! curl -fsSL -A "$UA" --max-time 20 --max-filesize 5000000 -o "$body" "$url" 2>/dev/null; then
        echo "  [skip] $url" >&2; rm -f "$body"; return
    fi

    # only parse HTML pages: first non-space, non-control byte must be '<'
    local first; first="$(head -c 1024 "$body" | LC_ALL=C tr -d '[:cntrl:] ' | head -c1)"
    if [ "$first" != "<" ]; then rm -f "$body"; return; fi

    local kind ref abs h
    while IFS="$TAB" read -r kind ref; do
        [ -z "$ref" ] && continue
        valid_ref "$ref" || continue
        abs="$(resolve_url "$url" "$ref")"
        if printf '%s' "${abs%%\?*}" | grep -qiE "$EXT_REGEX"; then
            download_image "$abs"                       # any image ref
        elif [ "$kind" = L ] && [ "$recursive" -eq 1 ] && [ "$depth" -lt "$max_depth" ]; then
            is_asset_ext "$abs" && continue             # never recurse into .js/.css/...
            h="$(printf '%s' "$abs" | sed -E 's#^[a-z]+://([^/]+).*#\1#')"
            [ "$h" = "$start_host" ] && crawl "$abs" $((depth+1))
        fi
    done < <(extract_refs < "$body")
    rm -f "$body"
}

# canonicalize the start URL the same way, so a host-only start matches the
# "scheme://host/" form its own page links to (otherwise recursion detours
# through the slash form and wastes a depth level).
start_url="$(normalize_url "$start_url")"

crawl "$start_url" 0
echo
echo "Done. $img_count image(s) saved to $(cd "$out" && pwd)"