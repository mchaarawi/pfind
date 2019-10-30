#!/bin/bash -e
# This script builds the optional parallel find

DAOS=/home/mschaara/install/daos
GURT=/home/mschaara/install/deps_daos/cart
CC="${CC:-mpicc}"
CFLAGS="-g -O2 -Wextra -Wall -pipe -std=gnu99 -Wno-format-overflow"
CFLAGS+="-I$DAOS/include -I$GURT/include"
LDFLAGS="-L$DAOS/lib -L$GURT/lib -lgurt -luuid -ldaos_common -ldaos -ldfs"

rm *.o 2>&1 || true

echo "Building parallel find;"

# Pfind can use lz4 to optimize the job stealing. 
# If you use ./prepare.sh it will try to download and compile lz4
if [[ -e ./lz4 ]] ; then
  echo "Using LZ4 for optimization"
  CFLAGS="$CFLAGS -DLZ4 -I./lz4/lib/"
  LDFLAGS="./lz4/lib/liblz4.a"
fi

$CC $CFLAGS -c src/pfind-main.c || exit 1
$CC $CFLAGS -c src/pfind-options.c || exit 1
$CC $CFLAGS -c src/pfind.c || exit 1
$CC $CFLAGS -o pfind *.o -lm $LDFLAGS || exit 1

echo "[OK]"
