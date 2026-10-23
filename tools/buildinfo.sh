#!/bin/sh
# Writes $1 (build/buildinfo.h): branch@commit (+dirty when runtime/, tools/ or the Makefile
# differ from HEAD), the build variant ($2: "lto", GENOPT flags) and the build time, shown on the
# bottom screen at boot (runtime/main.c).
g="git -c safe.directory=$(pwd) -c core.filemode=false -c core.autocrlf=true"
rev=$($g rev-parse --short HEAD 2>/dev/null || echo nogit)
br=$($g rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
$g diff --quiet HEAD -- runtime tools Makefile 2>/dev/null || rev="$rev+dirty"
[ -n "$2" ] && rev="$rev $2"   # build variant (Makefile: lto, GENOPT flags)
printf '#define RECOMP_BUILD "%s@%s %s"\n' "$br" "$rev" "$(date -u '+%Y-%m-%d %H:%M UTC')" > "$1"
