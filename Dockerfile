FROM ubuntu:25.04

# Install dependencies
RUN apt update && apt upgrade -y && \
    apt install -y \
        build-essential \
        cmake \
        git \
        libxrandr-dev \
        libxcursor-dev \
        libxi-dev \
        libudev-dev \
        libfreetype-dev \
        libflac-dev \
        libvorbis-dev \
        libgl1-mesa-dev \
        libegl1-mesa-dev \
        libfreetype-dev \
        libglib2.0-dev \ 
        libcairo2-dev \
        rapidjson-dev \
        libgl1-mesa-dev \
        libgtest-dev \
        libharfbuzz-dev \
        libboost-log-dev \
        x11-xserver-utils \
        && rm -rf /var/lib/apt/lists/*

WORKDIR /simple-engine-2/external

# Thor (DEPREACTED - Using custom resource manager)
# # Build and install Thor
# RUN git clone https://github.com/Bromeon/Thor.git && \
#     cd Thor && mkdir build && cd build && \
#     cmake .. && \
#     make -j$(nproc) && \
#     make install

# Install SFML
RUN git clone https://github.com/SFML/SFML.git && \
    cd SFML && mkdir build && \
    cmake -B build -S . -DBUILD_SHARED_LIBS=ON && \
    cmake --build build -j$(nproc) && \
    cmake --install build

# Install EnTT
RUN git clone https://github.com/skypjack/entt.git && \
    cd entt && \
    cmake -DENTT_INSTALL=ON -B build -S . && \
    cmake --build build -j$(nproc) && \
    cmake --install build

# ImGui + ImGui-SFML
RUN git clone -b docking --single-branch https://github.com/ocornut/imgui.git && \
    git clone https://github.com/SFML/imgui-sfml.git
    
# Create build directory
WORKDIR /simple-engine-2/build

# Create app workspace
WORKDIR /simple-engine-2

# Copy imconfig-SFML.h to imgui config
RUN cat external/imgui-sfml/imconfig-SFML.h >> external/imgui/imconfig.h

# Patch imgui-SFML.cpp to avoid assert on TexID
RUN sed -i 's/^.*assert(io\.Fonts->TexID.*$/\/\/ &/' /simple-engine-2/external/imgui-sfml/imgui-SFML.cpp

CMD ["bash"]



