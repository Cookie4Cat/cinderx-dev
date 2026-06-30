ARG PYTHON_VERSION=3.14.3
ARG PYPERFORMANCE_VERSION=1.13.0
ARG GCC_VERSION=14.2.0

FROM openeuler/openeuler:24.03-lts-sp3

# Re-declare ARGs so they are in scope for RUN/ENV instructions in this stage.
# They inherit the default values declared above FROM.
ARG PYTHON_VERSION
ARG PYPERFORMANCE_VERSION
ARG GCC_VERSION

ENV LANG=en_US.UTF-8

RUN dnf install -y \
    gcc \
    gcc-c++ \
    make \
    cmake \
    git \
    wget \
    curl \
    openssl-devel \
    zlib-devel \
    bzip2-devel \
    readline-devel \
    sqlite-devel \
    llvm \
    ncurses-devel \
    xz-devel \
    tk-devel \
    libxml2-devel \
    libffi-devel \
    gmp-devel \
    mpfr-devel \
    libmpc-devel \
    isl-devel \
    && dnf clean all

WORKDIR /tmp

RUN wget https://mirrors.huaweicloud.com/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz \
    && tar xf gcc-${GCC_VERSION}.tar.xz \
    && mkdir -p gcc-build \
    && cd gcc-build \
    && ../gcc-${GCC_VERSION}/configure --prefix=/usr/local/gcc-${GCC_VERSION} \
        --enable-languages=c,c++ \
        --disable-multilib \
        --with-system-zlib \
    && make -j$(nproc) \
    && make install \
    && cd /tmp \
    && rm -rf gcc-${GCC_VERSION} gcc-${GCC_VERSION}.tar.xz gcc-build

RUN ln -sf /usr/local/gcc-${GCC_VERSION}/bin/gcc /usr/local/bin/gcc \
    && ln -sf /usr/local/gcc-${GCC_VERSION}/bin/g++ /usr/local/bin/g++ \
    && ln -sf /usr/local/gcc-${GCC_VERSION}/bin/c++ /usr/local/bin/c++

ENV PATH=/usr/local/bin:$PATH

RUN wget https://mirrors.huaweicloud.com/python//${PYTHON_VERSION}/Python-${PYTHON_VERSION}.tgz \
    && tar xzf Python-${PYTHON_VERSION}.tgz \
    && cd Python-${PYTHON_VERSION} \
    && ./configure --enable-optimizations --with-lto \
    && make -j$(nproc) \
    && make install \
    && cd /tmp \
    && rm -rf Python-${PYTHON_VERSION} Python-${PYTHON_VERSION}.tgz

RUN python3 -m pip install --upgrade pip setuptools wheel

RUN pip3 install pyperformance==${PYPERFORMANCE_VERSION}

WORKDIR /workspace/cinderx

RUN python3 -m pyperformance venv create --inherit-environ http_proxy,https_proxy

RUN sed -i 's/^include-system-site-packages = false/include-system-site-packages = true/' venv/cpython*/pyvenv.cfg

COPY . /workspace/cinderx

ENV LDFLAGS="-Wl,-rpath,/usr/local/gcc-${GCC_VERSION}/lib64"

RUN pip3 install .

CMD ["/bin/bash"]
