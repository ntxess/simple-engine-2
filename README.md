# Simple Engine 2
Fork of the original simple game engine with new additions and structural changes

## Redesign in progress

The engine is being redesigned on the `redesign/engine-v2` branch: new targets
under `engine/` grow beside the legacy SFML tree (`core/`, `app/`), which is
kept as a deterministic behaviour reference. Start with
[docs/README.md](docs/README.md).

## Build (Windows, Visual Studio 18 / MSVC 14.51)

```bat
scripts\with-msvc.cmd cmake --workflow --preset msvc-dev
```

Local CI (build, tests, checks and lint on msvc-dev, msvc-asan and clangcl-dev):

```bat
powershell -File scripts\ci.ps1
```

The legacy editor builds with the `legacy-msvc-debug` and `legacy-msvc-release`
presets and runs from the repository root (`out/build/legacy-msvc-debug/app/editor_app.exe`).

# Running in Docker [WIP]
## Prerequisite:
xhost is required for docker to gain access to X11.

On Arch:
```bash
    sudo pacman -Syu xorg-xhost
```

On Ubuntu/Debian:
```bash
    sudo apt update
    sudo apt install x11-xserver-utils
```

## Build Docker image:

docker build -t simple-engine-2 .

## Grant docker access to X11:
xhost +local:docker 

## Run Docker container:
The docker container has external dependencies installed in ```simple-engine-2/external```

Make sure you are in the root directory of the ```simple-engine-2``` before running this:

```bash 
docker run -it -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix -v $(pwd)/app:/simple-engine-2/app -v $(pwd)/core:/simple-engine-2/core -v $(pwd)/assets:/simple-engine-2/assets -v $(pwd)/config:/simple-engine-2/config -v $(pwd)/log:/simple-engine-2/log -v $(pwd)/CMakeLists.txt:/simple-engine-2/CMakeLists.txt simple-engine-2 bash
```

## After finishing with docker:
Reminder to revoke Docker's access to X11 with:
```bash
xhost -local:docker
```