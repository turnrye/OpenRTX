FROM ubuntu:24.04

# Install dependencies
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
    wget ca-certificates sudo git pkg-config build-essential meson cmake libstdc++6 libusb-1.0-0 libusb-1.0-0-dev dfu-util libsdl2-dev libcodec2-dev libreadline-dev  python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Install moisix
RUN wget https://miosix.org/toolchain/MiosixToolchainInstaller.run \
    && sh MiosixToolchainInstaller.run

# Install radiotool
RUN wget https://github.com/v0l/radio_tool/releases/download/v0.3.0/radio_tool-0.3.0-Linux.deb \
    && dpkg -i radio_tool-0.3.0-Linux.deb

# Install opengd77 sources and put the firmware loader and tools in PATH
RUN git clone https://github.com/open-ham/OpenGD77.git /tmp/OpenGD77
ENV PATH="$PATH:/tmp/OpenGD77/tools/Python/FirmwareLoader:/tmp/OpenGD77/firmware/tools"

# Install Python 3.11 using the deadsnakes PPA
RUN apt-get update \
    && apt-get install -y software-properties-common \
    && add-apt-repository -y ppa:deadsnakes/ppa \
    && apt-get update \
    && apt-get install -y python3.11 python3.11-venv python3.11-dev \
    && rm -rf /var/lib/apt/lists/*

# Create and activate virtual environment
ENV VIRTUAL_ENV=/opt/venv
RUN python3.11 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

# Copy requirements.txt and install dependencies
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -U pip && \
    pip install --no-cache-dir -r requirements.txt

