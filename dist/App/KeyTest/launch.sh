#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
chmod +x "$DIR/keytest" 2>/dev/null
"$DIR/keytest" "$DIR"
