#!/bin/sh
# Makes Paper DJ start on every boot by adding one line to QNX's boot script
# (/system/etc/startup/post_startup.sh, which runs as root at the end of startup).
#   install:  sudo sh ~/paper-dj/install_autostart.sh
#   undo:     sudo sh ~/paper-dj/install_autostart.sh --remove
# The untouched original is kept as post_startup.sh.orig.

F=${PDJ_BOOT_SCRIPT:-/system/etc/startup/post_startup.sh}
DIR=$(cd "$(dirname "$0")" && pwd)
TAG="# Paper DJ"
LINE="sh $DIR/start.sh &   $TAG"

if [ "$(id -u)" != 0 ]; then echo "Run it with sudo: sudo sh $0 $*"; exit 1; fi
[ -f "$F" ] || { echo "Boot script not found: $F"; exit 1; }
TMP=/tmp/pdj_boot.$$

if [ "$1" = "--remove" ]; then
    # drop our line and the blank line we added after it
    awk -v t="$TAG" 'skip { skip = 0; if ($0 == "") next } index($0, t) { skip = 1; next } { print }' \
        "$F" > "$TMP" && cat "$TMP" > "$F"; rm -f "$TMP"
    echo "Removed. Paper DJ will no longer start at boot."
    exit 0
fi

if grep -q "$TAG" "$F"; then echo "Already installed:"; grep -n "$TAG" "$F"; exit 0; fi
[ -f "$F.orig" ] || cp -p "$F" "$F.orig"

# Insert just before the script's final "exit 0" (cat > keeps the file's owner and mode).
awk -v l="$LINE" '/^exit 0$/ && !done { print l; print ""; done = 1 } { print }' "$F" > "$TMP"
if grep -q "$TAG" "$TMP"; then
    cat "$TMP" > "$F"
    echo "Installed. Paper DJ will start on every boot:"
    grep -n "$TAG" "$F"
else
    echo "Couldn't find 'exit 0' in $F; nothing changed."
fi
rm -f "$TMP"
