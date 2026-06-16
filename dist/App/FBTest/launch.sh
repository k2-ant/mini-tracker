#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
chmod +x "$DIR/fbtest" 2>/dev/null
"$DIR/fbtest" "$DIR"
