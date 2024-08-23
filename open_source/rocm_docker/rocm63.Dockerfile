ARG BASE_OS_IMAGE
FROM $BASE_OS_IMAGE

MAINTAINER wangyin.yx

ADD functions /etc/rc.d/init.d/functions

RUN echo "ALL ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers && \
    groupadd sdev

RUN dnf install -y \
        unzip wget which findutils rsync tar \
        gcc gcc-c++ libstdc++-static gdb coreutils \
        binutils bash glibc-devel libdb glibc glibc-langpack-en bison lld \
        emacs-nox git git-lfs nfs-utils java-11-openjdk-devel docker \
        gcc-toolset-12 gcc-toolset-12-gcc-c++ libappstream-glib*

ARG AMD_BKC_URL
RUN wget $AMD_BKC_URL -O /tmp/bkc.tar.gz && \
    mkdir -p /tmp/bkc && \
    tar -xzvf /tmp/bkc.tar.gz -C /tmp/bkc && \
    cd /tmp/bkc/ali_8u && \
    bash install_ch.sh && \
    cd ./rpmpackages && \
    yum install -y *rand-* *blas* hipfft-* \
        miopen-hip-* hipsolver-* hipsparse* \
        composablekernel-devel-1.1.0.6030001-16.el8.x86_64.rpm \
        rocthrust-devel-3.0.1.6030001-16.el8.x86_64.rpm \
        hipcub-devel-3.2.0.6030001-16.el8.x86_64.rpm && \
    rm -rf /tmp/bkc /tmp/bkc.tar.gz

RUN git config --system core.hooksPath .githooks && \
    git lfs install

ENV PATH $PATH:/opt/conda310/bin:/opt/rocm/bin
ENV LD_LIBRARY_PATH $LD_LIBRARY_PATH:/opt/rh/gcc-toolset-12/root/usr/lib64:/lib64:/opt/conda310/lib/
RUN scl enable gcc-toolset-12 bash

ARG CONDA_URL
RUN wget $CONDA_URL -O /tmp/conda.sh && \
    sh /tmp/conda.sh -b -p /opt/conda310/ && \
    rm /tmp/conda.sh -f

ARG PYPI_URL
ADD deps /tmp/deps
RUN /opt/conda310/bin/pip install -r /tmp/deps/requirements_rocm.txt -i $PYPI_URL && \
    rm -rf /tmp/deps && pip cache purge

ARG BAZELISK_URL
RUN wget -q $BAZELISK_URL -O /usr/local/bin/bazelisk && chmod a+x /usr/local/bin/bazelisk