# Old-host build container: Debian "lenny" (2009, gcc 4.3) can compile the ancient
# gcc-3.4.5 + glibc-2.3.6 sources that a modern host (gcc >= 5) rejects.
FROM debian/eol:lenny
# lenny's apt repos are on archive.debian.org; allow expired release info
RUN printf 'Acquire::Check-Valid-Until "false";\n' > /etc/apt/apt.conf.d/10no-check-valid
RUN sed -i 's|http://ftp.*debian.org|http://archive.debian.org|g' /etc/apt/sources.list || true
RUN apt-get update && apt-get install -y --force-yes \
      build-essential gawk bison flex texinfo automake libtool \
      gettext wget patch file bzip2 gzip libncurses5-dev \
    && rm -rf /var/lib/apt/lists/*
# crosstool recipe (gcc-3.4.5-glibc-2.3.6, arm OABI soft-float) is mounted/run at build time;
# see toolchain/run_pathb.sh. Source tarballs are pre-seeded (crosstool's old FTP mirrors are dead).
WORKDIR /opt
