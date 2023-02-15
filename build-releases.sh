#! /bin/bash
set -euo pipefail

# Note: this _must_ be run on an Apple Silicon machine, since the macOS ARM build cannot be dockerised due to macOS license restrictions

# Get the tag associated with the latest release, to ensure parity between binaries
TAG_NAME=$(curl -sL https://api.github.com/repos/cloudflare/workerd/releases/latest | jq -r ".tag_name")

git checkout $TAG_NAME

# Build macOS binary
npx -p @bazel/bazelisk bazelisk build --disk_cache=./.bazel-cache -c opt //src/workerd/server:workerd

cp bazel-bin/src/workerd/server/workerd ./workerd-darwin-arm64

docker buildx build --platform linux/arm64 -f Dockerfile.release --target=artifact --output type=local,dest=$(pwd) .