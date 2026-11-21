#!/bin/sh
# Build release notes for commits since the previous tag.
# Usage: release_notes.sh [PREVIOUS_TAG] [OUTPUT]
set -eu

prev="${1:-}"
out="${2:-release-notes.md}"
repo="${GITHUB_REPOSITORY:-ChidcGithub/Finte}"

{
    echo "## Changes"
    echo
    if [ -n "$prev" ]; then
        echo "Since \`$prev\`:"
        echo
        git log --no-merges --pretty=format:'- %s (`%h`)' "$prev..HEAD"
    else
        echo "Initial release:"
        echo
        git log --no-merges --pretty=format:'- %s (`%h`)' HEAD
    fi
    echo
    echo
    echo "## Artifacts"
    echo
    echo "- \`finte.img\` — BIOS/UEFI hybrid USB image"
    echo "- \`finte.exe\` — native Windows host build"
    if [ -n "$prev" ]; then
        echo
        echo "**Full Changelog**: https://github.com/$repo/compare/$prev...HEAD"
    fi
} > "$out"

cat "$out"
