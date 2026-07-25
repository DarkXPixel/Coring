FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 \
    cmake \
    ninja-build \
    liburing-dev \
    pkg-config    \
    && rm -rf /var/lib/apt-get/lists/*

ENV CC=gcc-14
ENV CXX=g++-14

WORKDIR /src

COPY CMakeLists.txt ./
COPY src/ ./src/
COPY include/ ./include

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \ 
    && cmake --build build

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive


RUN apt-get update && apt-get install -y --no-install-recommends \
    liburing2 \
    libstdc++6 \
    && rm -rf /var/lib/apt-get/lists/*

WORKDIR /app
COPY --from=builder /src/build/exe /app/Coring

ENTRYPOINT [ "/app/Coring" ]