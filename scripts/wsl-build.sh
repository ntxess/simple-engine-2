#!/usr/bin/env bash
# Builds and tests one Linux workflow preset inside WSL.
#
#   scripts/wsl-build.sh linux-clang-tsan
#
# Building on /mnt/d is slow (9P), so the script keeps an ext4 clone at
# ~/se-wsl, fetches the current branch of the Windows checkout into it, and
# shares the pinned dependency archives through a symlink to .deps/.
#
# One-time setup (needs sudo; see docs/gates.md):
#   sudo apt install -y clang lld ninja-build python3-pip pipx glslang-tools
#   pipx install cmake            # Ubuntu 24.04 ships CMake 3.28; 3.31+ needed
set -euo pipefail

preset="${1:?usage: wsl-build.sh <workflow-preset>}"
win_repo="${SE_WIN_REPO:-/mnt/d/Dev/simple-engine-2}"
clone="${SE_WSL_CLONE:-$HOME/se-wsl}"
branch="$(git -C "$win_repo" rev-parse --abbrev-ref HEAD)"

if [ ! -d "$clone/.git" ]; then
  git clone --no-checkout "$win_repo" "$clone"
fi
git -C "$clone" fetch --force "$win_repo" "$branch:refs/remotes/win/$branch"
git -C "$clone" checkout --force -B "$branch" "refs/remotes/win/$branch"
if [ ! -e "$clone/.deps" ]; then
  ln -s "$win_repo/.deps" "$clone/.deps"
fi

cd "$clone"
cmake --workflow --preset "$preset"
