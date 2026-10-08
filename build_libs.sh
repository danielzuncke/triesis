#!/bin/bash

set -e

dir="$(cd "$(dirname "$0")" && pwd)"
cd "$dir"

build="$dir/tmp/build_libs" # TODO: remove (i think hnot used anymore)
libs="$dir/libs"

mkdir -p "$build" "$libs"


# abseil
# gets "installed" to libs/abseil
cmake -S submodules/abseil -B "$build/abseil" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_CXX_STANDARD=20 \
	-DCMAKE_CXX_FLAGS="-march=native" \
	-DABSL_PROPAGATE_CXX_STD=ON \
	-DABSL_BUILD_TESTING=OFF \
	-DCMAKE_INSTALL_PREFIX="$libs/abseil"
cmake --build "$build/abseil" -j $(nproc)
cmake --install "$build/abseil"


# wormhole
#
# flags copied from Makefile and Makefile.common
# kept the stack protector, I don't want the flags here to deviate too
# far from how the other data structs are compiled it devalues the test
cd submodules/wormhole
gcc -O2 -DNDEBUG -march=native -mtune=native -pthread \
  -fno-builtin-{memcpy,memmove,memcmp} \
  -std=gnu18 \
  -c {lib,kv,wh}.c
ar rcs libwh.a {lib,kv,wh}.o
mv libwh.a "$libs"
cd "$dir"


# marisa
#
# static lib via its cmake
cmake -S submodules/marisa -B "$build/marisa" \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=OFF \
	-DBUILD_TESTING=OFF \
	-DENABLE_TOOLS=OFF \
	-DENABLE_NATIVE_CODE=ON \
	-DCMAKE_INSTALL_PREFIX="$libs/marisa"
cmake --build "$build/marisa" -j $(nproc)
cmake --install "$build/marisa"


# cuckoo
#
# same flags, static etc.
# need to comment out multithreading for fair tests
cat<<EOF
cuckoo trie requires this for huge pages:
  sudo sh -c "echo 1000 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
EOF
cd "$dir/submodules/cuckoo"
sed -i 's|^#define MULTITHREADING|// #define MULTITHREADING|' config.h

for name in main util verify_trie random atomics mt_debug;
do
	gcc -O2 -DNDEBUG -march=native -fno-strict-aliasing -pthread \
	  -c "$name.c" -o "$name.o"
done
ar rcs "$libs/libcuckoo_trie.a" \
	{main,util,verify_trie,random,atomics,mt_debug}.o
cd "$dir"

echo "libs built into $libs"
