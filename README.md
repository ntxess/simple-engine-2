# Simple Engine 2
Fork of the original simple game engine with new additions and structural changes

## Build x64-Debug
```bash
cmake -S . -B "build/cmake/x64-Debug" -DCMAKE_BUILD_TYPE=Debug
```
```bash
cmake --build "build/cmake/x64-Debug" --config Debug -j
```
## Build x64-Release
```bash
cmake -S . -B "build/cmake/x64-Release" -DCMAKE_BUILD_TYPE=Release
```
```bash
cmake --build "build/cmake/x64-Release" --config Release -j
```
## Default
```bash
cmake --build . --config Release -j --clean-first
```

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