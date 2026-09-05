#!/bin/sh
set -eu

logger=${1:-./neoubxlogger}
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# Noise followed by B5 B5 62 must retain the second B5 as the frame sync.
# The two frames are a zero-payload frame and NAV-EOE without NAV-PVT.
printf '\265\265\142\001\002\000\000\003\012\265\142\001\141\004\000\000\000\000\000\146\307' |
	"$logger" -n -q 2>"$tmp"

if grep -q 'Invalid frame' "$tmp"; then
	cat "$tmp" >&2
	exit 1
fi
grep -q 'WASTED 1 Bytes' "$tmp"
grep -q 'Ignoring NAV-EOE without a valid NAV-PVT' "$tmp"

printf '\265\142\001\141\004\000\000\000\000\000\146\307' |
	"$logger" -n -d 2>"$tmp"
grep -q '(NAV-EOE, iTOW=0)' "$tmp"

# Exercise a fully generated std::format-based dump.
printf '\265\142\005\001\002\000\001\002\013\057' |
	"$logger" -n -d 2>"$tmp"
grep -q '(ACK-ACK, clsID=1, msgID=2)' "$tmp"

# Failed parsers and unsupported messages must retain their names and raw payload.
printf '\265\142\005\001\001\000\001\010\041' |
	"$logger" -n -d 2>"$tmp"
grep -q 'UBX-ACK-ACK (1)' "$tmp"
if grep -q "(ACK-ACK, clsID=0" "$tmp"; then
	exit 1
fi
printf '\265\142\023\000\001\000\001\025\143' |
	"$logger" -n -d 2>"$tmp"
grep -q 'UBX-MGA-GPS-EPH (1)' "$tmp"
printf '\265\142\006\212\004\000\000\000\000\000\224\016' |
	"$logger" -n -d 2>"$tmp"
grep -q 'UBX-CFG-VALSET (4)' "$tmp"
