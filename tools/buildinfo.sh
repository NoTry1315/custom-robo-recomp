#!/bin/sh
# Writes $1 (build/buildinfo.h): branch@commit (+dirty when runtime/, tools/ or the Makefile
# differ from HEAD), the build variant ($2: "lto", GENOPT flags) and the build time, shown on the
# bottom screen at boot (runtime/main.c). BUILD_BRANCH / BUILD_REV (environment, e.g. docker -e)
# replace the git lookup: a git worktree's .git points at a host path the build container cannot
# see, so worktree builds pass both from the host (BUILD_REV with its own +dirty).
g="git -c safe.directory=$(pwd) -c core.filemode=false -c core.autocrlf=true"
if [ -n "$BUILD_REV" ]; then
    rev=$BUILD_REV
else
    rev=$($g rev-parse --short HEAD 2>/dev/null || echo nogit)
    $g diff --quiet HEAD -- runtime tools Makefile 2>/dev/null || rev="$rev+dirty"
fi
br=${BUILD_BRANCH:-$($g rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)}
[ -n "$2" ] && rev="$rev $2"   # build variant (Makefile: lto, GENOPT flags)
printf '#define RECOMP_BUILD "%s@%s %s"\n' "$br" "$rev" "$(date -u '+%Y-%m-%d %H:%M UTC')" > "$1"
