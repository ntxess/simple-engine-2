#!/usr/bin/env bash
# Builds and tests one Linux workflow preset inside WSL.
#
#   scripts/wsl-build.sh linux-clang-tsan
#
# Building on /mnt/d is slow (9P), so the script keeps an ext4 clone, fetches
# a branch of the Windows repository into it, and shares the pinned
# dependency archives through a symlink to .deps/. Only committed work is
# built: the branch tip, not the working tree.
#
#   SE_WIN_REPO   the Windows checkout (default /mnt/d/Dev/simple-engine-2);
#                 worktree branches are fetched from it, since all worktrees
#                 share its refs
#   SE_BRANCH     the branch to build (default: the checkout's current one)
#   SE_WSL_CLONE  the ext4 clone (default ~/se-wsl); scripts/ci.ps1 gives
#                 each worktree branch its own
#
# One-time setup (needs sudo; see docs/gates.md):
#   sudo apt install -y clang-20 lld-20 libclang-rt-20-dev ninja-build \
#     python3-pip pipx glslang-tools   # clang 19+ for libstdc++ std::expected
#   pipx install cmake                 # Ubuntu 24.04 ships 3.28; 3.31+ needed
set -euo pipefail
# pipx installs CMake into ~/.local/bin, which only login shells put on PATH.
export PATH="$HOME/.local/bin:$PATH"

preset="${1:?usage: wsl-build.sh <workflow-preset>}"
win_repo="${SE_WIN_REPO:-/mnt/d/Dev/simple-engine-2}"
clone="${SE_WSL_CLONE:-$HOME/se-wsl}"
branch="${SE_BRANCH:-$(git -C "$win_repo" rev-parse --abbrev-ref HEAD)}"

if [ ! -d "$clone/.git" ]; then
  git clone --no-checkout "$win_repo" "$clone"
fi
export SE_DEPS_CACHE="${SE_DEPS_CACHE:-$win_repo/.deps}"
git -C "$clone" fetch --force "$win_repo" "$branch:refs/remotes/win/$branch"
git -C "$clone" checkout --force -B "$branch" "refs/remotes/win/$branch"
if [ ! -e "$clone/.deps" ]; then
  ln -s "$win_repo/.deps" "$clone/.deps"
fi

cd "$clone"
cmake --workflow --preset "$preset"
