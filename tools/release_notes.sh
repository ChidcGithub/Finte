#!/bin/sh
# Build release notes for commits since the previous tag.
# Usage: release_notes.sh [PREVIOUS_TAG] [OUTPUT]
set -eu

prev="${1:-}"
out="${2:-release-notes.md}"
current="${3:-HEAD}"
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
    echo "- \`finte-x86_64.img\` — x86_64 BIOS/UEFI hybrid USB image"
    echo "- \`finte-x86_64.iso\` — x86_64 BIOS/UEFI hybrid ISO"
    echo "- \`finte-arm64.img\` — arm64 UEFI disk image"
    echo "- \`finte-arm64.iso\` — arm64 UEFI ISO"
    echo "- \`finte.exe\` — native Windows host build"
    if [ -n "$prev" ]; then
        echo
        echo "**Full Changelog**: https://github.com/$repo/compare/$prev...$current"
    fi
} > "$out"

cat "$out"
