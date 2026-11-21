#!/bin/sh
# Tag the current commit and push only the tag. The release workflow runs on
# tag pushes, not on ordinary main-branch pushes.
set -eu

cd "$(dirname "$0")/.."

if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "working tree is not clean" >&2
    exit 1
fi

hash=$(git rev-parse --short=7 HEAD)
version="${1:-0.0.1-$hash}"
case "$version" in
    v*) tag="$version" ;;
    *)  tag="v$version" ;;
esac

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "tag already exists: $tag"
else
    git tag "$tag"
    echo "created $tag"
fi

git push origin "$tag"
