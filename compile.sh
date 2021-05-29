#!/bin/bash -e
# This script builds the optional parallel find

DAOS=${MY_DAOS_INSTALL_PATH}
MFU=${MY_MFU_INSTALL_PATH}

CC="${CC:-mpicc}"
CFLAGS="-g -O2 -Wextra -Wall -pipe -std=gnu99 -Wno-format-overflow -I$MFU/include/"
#LDFLAGS="$MFU/lib64/libmfu_dfind-static.a $MFU/lib64/libmfu.a $MFU/lib/libcircle.a $MFU/lib/libdtcmp.a $MFU/lib/liblwgrp.a"
#LDFLAGS="$LDFLAGS $DAOS/lib64/libdfs.so $DAOS/lib64/libdaos_common.so $DAOS/lib64/libdaos.so $DAOS/lib64/libgurt.so -luuid"
LDFLAGS2="-L$MFU/lib64/ -lmfu_dfind -lmfu"
LDFLAGS2="$LDFLAGS2 -L$DAOS/lib64/ -ldfs -ldaos -ldaos_common -lgurt -luuid"

rm *.o *.a 2>&1 || true

echo "Building parallel find;"

# Pfind can use lz4 to optimize the job stealing.
# If you use ./prepare.sh it will try to download and compile lz4
if [[ -e ./lz4 ]] ; then
  echo "Using LZ4 for optimization"
  CFLAGS="$CFLAGS -DLZ4 -I./lz4/lib/"
  LDFLAGS="$LDFLAGS ./lz4/lib/liblz4.a"
#  LDFLAGS="$LDFLAGS -L./lz4/lib/ -llz4"
fi

$CC $CFLAGS -c src/pfind-main.c $LDFLAGS2 || exit 1
$CC $CFLAGS -c src/pfind-options.c $LDFLAGS2 || exit 1
$CC $CFLAGS -c src/pfind.c $LDFLAGS2 || exit 1
$CC $CFLAGS -o pfind *.o -lm $LDFLAGS $LDFLAGS2|| exit 1
ar rcsT pfind.a pfind-options.o pfind.o $LDFLAGS
#$CC -shared -o pfind.so *.o -lm $LDFLAGS

echo "[OK]"
