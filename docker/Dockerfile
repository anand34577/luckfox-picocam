# Luckfox Pico (RV1103/RV1106) AI build environment
#  - RKNN-Toolkit2 (ONNX -> .rknn conversion, INT8 quantization)
#  - Luckfox uClibc cross-compiler (arm-rockchip830-linux-uclibcgnueabihf)
#  - rknn_model_zoo (ready-made models + C++ demos)
#  - Go + Node (server / web UI builds)
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG RKNN_VER=2.5.0
ARG GO_VER=1.24.4
ARG NODE_MAJOR=20

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip python3-dev build-essential cmake ninja-build pkg-config \
        git git-lfs curl wget ca-certificates unzip xz-utils file rsync openssh-client \
        libgl1 libglib2.0-0 libsm6 libxext6 libxrender1 \
    && rm -rf /var/lib/apt/lists/*

# Go
RUN curl -fsSL https://go.dev/dl/go${GO_VER}.linux-amd64.tar.gz | tar -C /usr/local -xz
ENV PATH=/usr/local/go/bin:/root/go/bin:$PATH

# Node
RUN curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash - \
    && apt-get install -y --no-install-recommends nodejs && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# RKNN-Toolkit2 (+ runtime libs/headers for armhf-uclibc). Only the needed files are
# fetched; the full repo is several GB.
ARG RKNN_RAW=https://github.com/airockchip/rknn-toolkit2/raw/master
RUN mkdir -p /opt/rknpu2/include /opt/rknpu2/armhf-uclibc && cd /tmp \
    && WHL=rknn_toolkit2-${RKNN_VER}-cp310-cp310-manylinux_2_17_x86_64.manylinux2014_x86_64.whl \
    && curl -fsSLO ${RKNN_RAW}/rknn-toolkit2/packages/x86_64/requirements_cp310.txt \
    && curl -fsSLO ${RKNN_RAW}/rknn-toolkit2/packages/x86_64/${WHL} \
    && pip3 install --no-cache-dir -r requirements_cp310.txt ./${WHL} "setuptools<70" onnx onnxsim \
    && rm -f /tmp/*.whl /tmp/*.txt \
    && for f in rknn_api.h rknn_custom_op.h rknn_matmul_api.h; do \
         curl -fsSL -o /opt/rknpu2/include/$f ${RKNN_RAW}/rknpu2/runtime/Linux/librknn_api/include/$f; done \
    && curl -fsSL -o /opt/rknpu2/armhf-uclibc/librknnmrt.so ${RKNN_RAW}/rknpu2/runtime/Linux/librknn_api/armhf-uclibc/librknnmrt.so \
    && python3 -c "from rknn.api import RKNN; print('rknn-toolkit2 OK')"

# Luckfox uClibc cross toolchain (sparse checkout of the SDK)
RUN git clone --depth 1 --filter=blob:none --sparse https://github.com/LuckfoxTECH/luckfox-pico.git /tmp/sdk \
    && cd /tmp/sdk && git sparse-checkout set tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf \
    && mv tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf /opt/toolchain \
    && rm -rf /tmp/sdk
ENV PATH=/opt/toolchain/bin:$PATH
ENV CROSS=arm-rockchip830-linux-uclibcgnueabihf-
ENV GCC_COMPILER=/opt/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf

# Model zoo (ready-made models + demo sources)
RUN git clone --depth 1 https://github.com/airockchip/rknn_model_zoo.git && rm -rf rknn_model_zoo/.git

# Luckfox RKMPI SDK: camera/ISP/encoder/RTSP headers + uClibc libs (used by picocam/firmware/CMakeLists.txt)
RUN git clone --depth 1 https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example.git /opt/rkmpi \
    && rm -rf /opt/rkmpi/.git /opt/rkmpi/install /opt/rkmpi/images /opt/rkmpi/lib/glibc

# Model export: Ultralytics (YOLOv8/11) and Rockchip's YOLOv5 fork
RUN pip3 install --no-cache-dir ultralytics==8.3.40 ipython \
    && git clone --depth 1 https://github.com/airockchip/yolov5.git /opt/rk_yolov5 && rm -rf /opt/rk_yolov5/.git

# project helper scripts (models/*.py) are mounted at /work; build the firmware with:
#   docker run --rm -v "$PWD":/work -w /work/picocam luckfox-ai:1.1 sh build.sh all

RUN ${CROSS}gcc --version | head -1 && go version && node --version

WORKDIR /work
CMD ["/bin/bash"]
