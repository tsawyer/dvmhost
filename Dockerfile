FROM --platform=linux/amd64 debian:12

# Install dependencies once and bake them into the image
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    tar \
    ccache \
    libasio-dev \
    libncurses-dev \
    libssl-dev \
    time \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
