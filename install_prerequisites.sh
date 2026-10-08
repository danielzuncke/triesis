#!/bin/bash

set -e

# sys
sudo apt-get update
sudo apt-get -y install \
	git \
	gcc \
	g++ \
	make \
	cmake \
	pkg-config

# perfevent
sudo apt-get -y install linux-libc-dev

# CoCo
sudo apt-get -y install libboost-dev

# test env
failed=

check_hugepages() {
	if ! mount | grep /dev/shm | grep 'huge=always' >/dev/null; then
		cat<<EOF >&2
hugepages are not set up for /dev/shm
  sudo mount -o remount,huge=always /dev/shm
For persistent settings use (might also want nosuid,mode=1777,...):
  tmpfs /dev/shm tmpfs defaults,huge=always 0 0
EOF
		failed=1
	fi
}

# empty output otherwise (cannot measure but doesnt crash program)
check_perfevent() {
	local paranoid_file=/proc/sys/kernel/perf_event_paranoid
	if [ ! -r "$paranoid_file" ]; then
		echo "$paranoid_file not found: kernel has no perf events support" >&2
		failed=1
		return
	fi
	local paranoid
	paranoid="$(cat "$paranoid_file")"
	if [ "$paranoid" -gt 1 ]; then
		cat<<EOF >&2
kernel.perf_event_paranoid is $paranoid, perfevent needs <= 1
  sudo sysctl -w kernel.perf_event_paranoid=1
For persistent settings use:
  echo 'kernel.perf_event_paranoid = 1' | sudo tee /etc/sysctl.d/99-perfevent.conf
EOF
		failed=1
	fi
}

# not using hugepages with custom loader for official thesis tests, ignore func
# check_hugepages
check_perfevent

if [ -n "$failed" ]; then
	exit 1
fi
echo "prerequisites ok"
