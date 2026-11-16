#!/usr/bin/env bash
set -euo pipefail
publish=false
prerelease=false
tag=''
version="snapshot-${GITHUB_SHA:0:12}"
if [[ "$GITHUB_EVENT_NAME" == push && "$GITHUB_REF_TYPE" == tag ]]; then
  publish=true
  tag="$GITHUB_REF_NAME"
elif [[ "$GITHUB_EVENT_NAME" == workflow_dispatch && "$REQUEST_PUBLISH" == true ]]; then
  publish=true
  tag="$REQUEST_TAG"
fi
if [[ "$publish" == true ]]; then
  if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.-]+)?$ ]]; then
    echo '::error::Publishing requires a version tag such as v12.0.0 or v12.0.0-beta.1'
    exit 1
  fi
  if [[ "$(git rev-parse "refs/tags/$tag^{commit}")" != "$GITHUB_SHA" ]]; then
    echo '::error::The release tag must point at the workflow commit'
    exit 1
  fi
  version="$tag"
  if [[ "$tag" == *-* || "$REQUEST_PRERELEASE" == true ]]; then
    prerelease=true
  fi
fi
echo "version=$version" >> "$GITHUB_OUTPUT"
echo "tag=$tag" >> "$GITHUB_OUTPUT"
echo "publish=$publish" >> "$GITHUB_OUTPUT"
echo "prerelease=$prerelease" >> "$GITHUB_OUTPUT"
