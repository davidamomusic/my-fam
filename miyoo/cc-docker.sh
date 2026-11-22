#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The Miyoo Mini's cross compiler from its Docker image, as a plain compiler for tools/build.py:
#   CC=miyoo/cc-docker.sh python3 tools/build.py fam --miyoo
# The paths are absolute, so the working tree is mounted at the same place in the container.
# MIYOO_IMAGE: the toolchain image (default aemiii91/miyoomini-toolchain, Onion's); MIYOO_GCC: the compiler in it.
IMAGE="${MIYOO_IMAGE:-aemiii91/miyoomini-toolchain:latest}"
GCC="${MIYOO_GCC:-/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec docker run --rm -v "$ROOT:$ROOT" -w "$(pwd)" "$IMAGE" "$GCC" "$@"
