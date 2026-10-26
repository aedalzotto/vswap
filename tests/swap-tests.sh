#!/usr/bin/env bash
# Swap device tests for /dev/vswap0.
#   ./swap-tests.sh integrity     # 3 GiB through a 512M cgroup, SHA-256 check (root only to set ZSWAP)
#   sudo ./swap-tests.sh stress   # 10 min of swap churn inside an 8 GiB cgroup
#   sudo ./swap-tests.sh speed    # swapoff $DEV, fio, swapon again (zswap plays no part here)
# DEV=/dev/xyz overrides the swap device (default /dev/vswap0).
# ZSWAP=on|off sets zswap for the run and restores it afterwards; unset leaves it as it is.
# Every run ends with how much went to zswap and how much really reached a swap device.
# Logs: ./logs/<date>-<test>-zswap<Y|N>/
set -uo pipefail
cd "$(dirname "$0")"
T=${1:-} DEV=${DEV:-/dev/vswap0} SSD=${SSD:-/dev/nvme0n1}  # SSD: root disk, read-only fio only
Z=/sys/module/zswap/parameters/enabled zold=$(cat $Z 2>/dev/null || echo N)
[[ $T =~ ^(integrity|stress|speed)$ ]] || { echo "usage: [DEV=..] [ZSWAP=on|off] $0 integrity|stress|speed" >&2; exit 1; }
[[ $EUID -eq 0 || ($T == integrity && -z ${ZSWAP:-}) ]] || { echo "run as root" >&2; exit 1; }

case ${ZSWAP:-} in
on) zs=Y ;; off) zs=N ;; "") zs=$zold ;;
*) echo "ZSWAP must be on or off" >&2; exit 1 ;;
esac
L=logs/$(date +%F-%H%M)-$T-zswap$zs; mkdir -p "$L" || { echo "cannot write $L (root-owned logs/? chown it)" >&2; exit 1; }
[[ -n ${ZSWAP:-} ]] && echo $zs >$Z

# swap traffic counters (4 KiB pages): pswp* = really hit a swap device, zswp* = handled by zswap
vm() { awk '/^(pswpin|pswpout|zswpin|zswpout|zswpwb) /{printf "%s=%s ", $1, $2}' /proc/vmstat; }
v0=$(vm)
report() {
	local a b
	declare -A A B
	for a in $v0; do A[${a%=*}]=${a#*=}; done
	for b in $(vm); do B[${b%=*}]=${b#*=}; done
	mib() { echo $(( (B[$1] - A[$1]) * 4 / 1024 )); }
	echo "== $T, zswap=$zs: device out $(mib pswpout) MiB, in $(mib pswpin) MiB;" \
		"zswap stored $(mib zswpout) MiB, loaded $(mib zswpin) MiB, wrote back $(mib zswpwb) MiB" | tee $L/summary.log
}
restore=:
trap 'report; [[ -n ${ZSWAP:-} ]] && echo $zold >$Z; eval "$restore"' EXIT

case $T in
integrity)
	scope=(systemd-run --scope -q -p MemoryMax=512M); [[ $EUID -eq 0 ]] || scope=(systemd-run --user --scope -q -p MemoryMax=512M)
	"${scope[@]}" python3 integrity.py 2>&1 | tee $L/integrity.log ;;
stress)
	old=$(sysctl -n vm.swappiness)
	dmesg -c >/dev/null
	# 1 sample/s; gaps in timestamps = freezes
	( while :; do echo "$(date +%T) $(grep -E '^pswp(in|out)' /proc/vmstat | tr '\n' ' ') $(awk 'NR>1{printf "%s=%s ",$1,$4}' /proc/swaps)"; sleep 1; done ) >$L/stress-vmstat.log & mon=$!
	restore='sysctl -q vm.swappiness=$old; kill $mon 2>/dev/null'
	sysctl -q vm.swappiness=180
	# 12 GiB of live pages (--vm-bytes is the total, split across the 4 workers) in an 8 GiB cgroup:
	# ~4 GiB churns through swap the whole time without starving the desktop.
	systemd-run --scope -q -p MemoryMax=8G -p MemorySwapMax=infinity \
		stress-ng --vm 4 --vm-bytes 12g --vm-keep -t 10m --metrics-brief 2>&1 | tee $L/stress-ng.log
	sleep 30   # recovery window
	{ free -h; swapon --show; cat /proc/pressure/memory; } | tee $L/stress-after.log
	dmesg | tee $L/stress-dmesg.log | grep -iE 'amdgpu|vswap|oom|error|hung' ;;
speed)
	swapoff $DEV || exit 1
	restore='mkswap -q $DEV && swapon -p 100 $DEV; swapon --show'
	# f <rw> <dev> <iodepth> [extra fio args]; swap-in is mostly QD1, so read at both depths
	f() { echo "== $1 $2 QD$3"; fio --name=$1 --filename=$2 --rw=$1 --bs=4k --direct=1 --iodepth=$3 --ioengine=libaio --runtime=30 --time_based ${4:-} | tee $L/fio-$1-qd$3-$(basename $2).log | grep -E 'IOPS|lat \(usec\): min|clat.*avg'; }
	f randread $DEV 1; f randread $DEV 32; f randwrite $DEV 32
	f randread $SSD 1 --readonly; f randread $SSD 32 --readonly ;;
esac
