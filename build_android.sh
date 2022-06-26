#!/bin/bash
export NDK_ROOT=/opt/android-ndk-r23c
export PATH="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/:$PATH"

export CXX="aarch64-linux-android31-clang++"
export CC="aarch64-linux-android31-clang"
export AR="llvm-ar"

./configure \
  --host=aarch64-linux-android31 \
  --without-x \
  --prefix=/data/local/lirc \
  --sysconfdir=/data/local \
  --localstatedir=/data/local/lirc/var \
  --with-lockdir=/data/local/lirc/var

make
