#!/bin/bash -e
# This script builds the parallel find with the DAOS DFS (pipeline) backend

DAOS=${MY_DAOS_INSTALL_PATH:-${DAOS_INSTALL_PATH:-/usr}}

# Regular options, set on the command line CC
CC="${CC:-mpicc}"
CFLAGS="-g -O2 -Wextra -Wall -pipe -std=gnu99 -Wno-format-overflow -I$DAOS/include"
LDFLAGS="-L$DAOS/lib64 -L$DAOS/lib -Wl,-rpath,$DAOS/lib64 -Wl,-rpath-link,$DAOS/lib64 -ldfs -ldaos -lgurt"

# Test settings
#CC="${CC:-clang-10}"
#CFLAGS="-g -O2 -Wextra -Wall -pipe -std=gnu99 -Wno-format-overflow $(mpicc -showme:compile)"
#LDFLAGS="$(mpicc -showme:link)"

rm *.o *.a 2>&1 || true

echo "Building parallel find;"

FILES=""
# Pfind can use lz4 to optimize the job stealing.
# If you use ./prepare.sh it will try to download and compile lz4
if [[ -e ./lz4 ]] ; then
  echo "Using LZ4 for optimization"
  CFLAGS+=" $CFLAGS -DLZ4 -I./lz4/lib/"
  LDFLAGS+=" ./lz4/lib/liblz4.a"
  FILES+=./lz4/lib/*.a
fi

$CC $CFLAGS -c src/pfind-main.c || exit 1
$CC $CFLAGS -c src/pfind-options.c || exit 1
$CC $CFLAGS -c src/pfind.c || exit 1
$CC $CFLAGS -c src/pfind-dfs.c || exit 1
$CC $CFLAGS -c src/option.c || exit 1
$CC $CFLAGS -o pfind *.o -lm $LDFLAGS || exit 1
ar rcsT pfind.a option.o pfind-options.o pfind.o pfind-dfs.o $FILES

echo "[OK]"
