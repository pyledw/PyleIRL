FROM lscr.io/linuxserver/obs-studio:latest

# Update repositories and install build dependencies, 
# then clone the PyleIRL repository, compile, and install the plugin,
# finally clean up the build dependencies to keep the image lightweight.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    git \
    build-essential \
    cmake \
    libobs-dev \
    qt6-base-dev \
    libqt6core5compat6-dev \
    pkg-config \
    ca-certificates && \
    cd /tmp && \
    git clone https://github.com/pyledw/PyleIRL.git && \
    cd PyleIRL && \
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --config Release && \
    cmake --install build --prefix /usr && \
    cd / && \
    rm -rf /tmp/PyleIRL && \
    apt-get purge -y git build-essential cmake qt6-base-dev pkg-config libqt6core5compat6-dev && \
    apt-get autoremove -y && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*
