#!/bin/bash
# Runs INSIDE the old-host container. Builds gcc-3.4.5 + glibc-2.3.6 arm OABI soft-float.
set -e
export DEBIAN_FRONTEND=noninteractive
# Fix apt sources for EOL release if needed, then install build deps
if command -v apt-get >/dev/null 2>&1; then
  apt-get update -y 2>/dev/null || true
  apt-get install -y --no-install-recommends \
     build-essential bison flex gawk texinfo patch file bzip2 gzip xz-utils \
     libncurses5-dev autoconf automake gettext 2>/dev/null || \
  apt-get install -y build-essential bison flex gawk texinfo patch file bzip2 gzip 2>/dev/null || true
fi
cd /work/kegel/crosstool
export TARBALLS_DIR=/work/downloads
export RESULT_TOP=/work/x-tools-gcc345
export GCC_LANGUAGES="c,c++"
export USER=builder           # bypass crosstool's root-check (rootless podman maps root->host user)
export NO_DOWNLOAD=1          # we pre-seeded all tarballs; don't hit dead ftp mirrors
mkdir -p "$RESULT_TOP"
# OABI soft-float target per arm-softfloat.dat + gcc-3.4.5-glibc-2.3.6.dat
eval `cat arm-softfloat.dat gcc-3.4.5-glibc-2.3.6.dat` sh all.sh --notest
echo "CROSSTOOL_EXIT=$?"
