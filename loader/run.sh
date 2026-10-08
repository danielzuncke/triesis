#!/bin/bash

dir="$(cd "$(dirname "$0")" && pwd)"

# make filepaths abs before cd'ing to $dir
# use -- before realpath since we might give flags to dub etc.
args=()
for arg in "$@"; do
	if [ -f "$arg" ]; then
		args+=("$(realpath -- "$arg")")
	else
		args+=("$arg")
	fi
done

cd "$dir"

is_hugepages() {
	if ! mount | grep /dev/shm | grep 'huge=always' >/dev/null; then
		echo "hugepages are not set up. stop." >&2
		exit 1
	fi
}

is_hugepages

is_testset=
for arg in "${args[@]}"; do
	if [ "$arg" = "testset" ]; then
		is_testset="yes"
	fi
done
if [ -z "$is_testset" ]; then
	rm /dev/shm/dataset.bin || true
	rm /dev/shm/dataset_meta.bin || true
else
	rm /dev/shm/testset.bin || true
	rm /dev/shm/testset_meta.bin || true
fi

dub run --compiler=ldc2 --build=release --quiet "${args[@]}"
