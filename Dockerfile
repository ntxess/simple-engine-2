FROM ubuntu:22.04

# Install dependencies
RUN apt update && \
    apt install -y \
        build-essential \
        cmake \
        git \
        libboost-all-dev \
        libsfml-dev \
        rapidjson-dev \
        libgl1-mesa-dev \
        libgtest-dev \
        && rm -rf /var/lib/apt/lists/*

WORKDIR /simple-engine-2/external

# Build and install Thor
RUN git clone https://github.com/Bromeon/Thor.git && \
    cd Thor && mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc) && \
    make install

# Install EnTT
RUN git clone https://github.com/skypjack/entt.git && \
    cd entt && \
    cmake -DENTT_INSTALL=ON -B build -S . && \
    cmake --build build && \
    cmake --install build

# ImGui + ImGui-SFML
RUN git clone -b docking --single-branch https://github.com/ocornut/imgui.git && \
    git clone https://github.com/SFML/imgui-sfml.git

# Create build directory
WORKDIR /simple-engine-2/build

# Create app workspace
WORKDIR /simple-engine-2

CMD ["bash"]



