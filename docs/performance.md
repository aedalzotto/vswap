# vswap performance

Measured on Radeon RX 6800 (Navi 21, 16 GiB, Resizable BAR, PCIe link at 8 GT/s x16), XPG S70 NVMe SSD, 32 GiB RAM, Linux 6.18 (CachyOS), RADV.

## Summary

A 4 KiB swap-in from vswap costs about 8 µs of copy when the GPU memory clock is up, against 63 µs for the SSD at queue depth 1. 
The SSD wins at high queue depth, because CPU reads over the BAR top out around 600 MiB/s.

## Block device, fio (`tests/swap-tests.sh speed`)

4 KiB random I/O, `O_DIRECT`, 30 s.

| Test | vswap0 | SSD |
| --- | --- | --- |
| Read, QD1 | 24 µs, 160 MiB/s | 63 µs, 61 MiB/s |
| Read, QD32 | 167 MiB/s | 950 MiB/s |
| Write, QD32 | 1403 MiB/s | (not measured) |

Swap-in is mostly QD1: a page fault waits for one page. 
With `vm.page-cluster` at 0, as on this machine, each fault reads exactly one page.

## Where a read's time goes

| Component | Time per 4 KiB |
| --- | --- |
| Block layer + fio (measured on `brd`) | ~1 µs |
| Reader semaphore | 49 ns |
| `kernel_fpu_begin/end` | 48 ns |
| Copy from VRAM, memory clock 1000 MHz | 7.5 µs |
| Copy from VRAM, memory clock 96 MHz | 38 µs (up to 150 µs seen) |
| Copy with plain loads (`memcpy_fromio`) | 152 µs |

The 24 µs fio average above was taken at an in-between clock state.

## GPU memory clock

BAR reads do not count as GPU activity, so the memory clock does not rise for them. 
When the screen is on it usually sits at the top level anyway.
When the screen is off it drops to the bottom one.

| Memory clock (forced) | 4 KiB read | GPU idle power |
| --- | --- | --- |
| 96 MHz | 38 µs | 10 W |
| 456 MHz | 9.1 µs | 27 W |
| 673 MHz | 8.5 µs | 28 W |
| 1000 MHz | 7.5 µs | 34 W |

Holding the clock up while swap is active is left for later.

## Concurrency

Userspace benchmark, random 4 KiB reads from a Vulkan mapping:

| Threads | `movntdqa` |
| --- | --- |
| 1 | 520–586 MiB/s |
| 2 | 580–628 MiB/s |
| 4 | 236–311 MiB/s |
| 16 | 20–260 MiB/s |

More than two concurrent readers make things worse, hence `max_readers=2`.
Sequential writes from the CPU reach 3460 MiB/s.

## Stress (`tests/swap-tests.sh stress`)

12 GiB of live pages in an 8 GiB cgroup for 10 minutes, zswap off, `vm.swappiness=180`.

| Metric | Result |
| --- | --- |
| Written to vswap0 | 131 GiB (213 MiB/s) |
| Read from vswap0 | 127 GiB (206 MiB/s) |
| Monitoring gaps over 2 s (freezes) | none |
| stress-ng failures | 0 |
| dmesg | clean |

## zswap

zswap is on by default in CachyOS. 
It keeps pages compressed in RAM and only writes back to swap devices when its pool fills, so with zswap on vswap barely sees traffic (`pswpout` stays flat while `/proc/swaps` shows slots "used").
The numbers above were taken with zswap off. 
Both together are a reasonable daily setup: zswap keeps what compresses well in RAM and vswap takes the rest.
Example with zswap on: the integrity test's 3 GiB of random data still reached vswap through zswap writeback (5647 MiB written back, 0 mismatches).

## How to measure

- `tests/swap-tests.sh speed | integrity | stress`: fio against the device, a 3 GiB SHA-256 round trip through a 512 MiB cgroup, and the stress run above.
`ZSWAP=on|off` sets zswap for the run (and restores it).
Each run ends with a line saying how many MiB went to zswap and how many really reached a swap device, so results with and without zswap can be reported side by side.
- `sudo tests/vswap-lat.bt`: bpftrace histograms of time spent in `vswap_submit_bio`, per direction and I/O size. 
It attaches through the module's BTF, so the module carries no measurement code and costs nothing when nobody traces.
- `tests/vram-read-bench.c`: userspace VRAM read benchmark through Vulkan, no root needed.
- Check the clock while measuring: `cat /sys/class/drm/card0/device/pp_dpm_mclk`.
