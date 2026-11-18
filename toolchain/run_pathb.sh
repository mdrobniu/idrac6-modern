#!/bin/bash
cd /work/kegel/crosstool
export TARBALLS_DIR=/work/downloads
export RESULT_TOP=/work/x-tools-gcc345
export GCC_LANGUAGES="c,c++"
export USER=builder
export NO_DOWNLOAD=1
mkdir -p "$RESULT_TOP"
eval `cat arm-softfloat.dat gcc-3.4.5-glibc-2.3.6.dat` sh all.sh --notest
