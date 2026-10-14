#!/bin/sh
# Download the Windows build tools the release ships in tools\, pinned by
# version and SHA-256, into a cache folder (default ~/.cache/breakdown-win-toolchain).
# The cache keeps Breakdown's name on purpose: the pins are identical, so both
# titles' releases share one download.
# Official sources only: python.org (embeddable Python), ziglang.org (zig),
# PyPI (capstone, cmake and ninja wheels for win_amd64).
set -e
CACHE=${CACHE:-$HOME/.cache/breakdown-win-toolchain}
PIP=${PIP:-/mnt/dev/xboxrecomp-fork/.venv/bin/pip}
mkdir -p "$CACHE/wheels"
cd "$CACHE"
[ -f python-3.14.7-embed-amd64.zip ] || curl -fsSLO https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip
[ -f zig-x86_64-windows-0.14.1.zip ] || curl -fsSLO https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip
"$PIP" download --quiet --platform win_amd64 --only-binary=:all: --python-version 3.14 \
    -d wheels capstone==5.0.9 cmake==4.4.4 ninja==1.13.2
sha256sum -c <<'SUMS'
d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15  python-3.14.7-embed-amd64.zip
554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c  zig-x86_64-windows-0.14.1.zip
732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0  wheels/capstone-5.0.9-py3-none-win_amd64.whl
ad8e0a38b5707e27882701146bdfffecacd80e7703fa0fc36c77528518d545af  wheels/cmake-4.4.4-py3-none-win_amd64.whl
1293f4078278b70d0ee4b6cc8f3a9e030656c9b2f59909970343c4fe76070118  wheels/ninja-1.13.2-py3-none-win_amd64.whl
SUMS
