# Simple Engine 2
Fork of the original simple game engine with new additions and structural changes

# Dockerfile
docker build -t simple-engine-2

The docker container has dependencies installed in '/' and 'simple-engine-2/external'
Make sure you are in the root directory of the project before running this:

docker run -it -v $(pwd)/app:/simple-engine-2/app -v $(pwd)/core:/simple-engine-2/core -v $(pwd)/assets:/simple-engine-2/assets -v $(pwd)/config:/simple-engine-2/config -v $(pwd)/CMakeLists.txt:/simple-engine-2/CMakeLists.txt simple-engine-2 bash

