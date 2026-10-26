# vswap

Unused VRAM is wasted money, so if you are not using it, this module allows the kernel to page out to the GPU's memory before it touches the SSD.

## About

Unlike [vramfs](https://github.com/Overv/vramfs), which serves swap through a FUSE daemon and can freeze under memory pressure, vswap does every swap-in and swap-out inside the kernel. 
Nothing in that path allocates memory, talks to the GPU or waits on a userspace process.

On an RX 6800 a 4 KiB swap-in takes about 8 µs against 63 µs for an NVMe SSD. 
See [docs/performance.md](docs/performance.md).

## Requirements

- Mesa Vulkan driver with `VK_EXT_external_memory_dma_buf` (only tested on AMD RX 6800);
- Resizable BAR enabled;
- Linux 6.13 or newer;

## Build

```
make
sudo make install
```

## Quick start

```
sudo modprobe vswap
sudo vswap attach 12G
sudo mkswap /dev/vswap0
sudo swapon -p 100 /dev/vswap0
```

> Note that `-p 100` is used so vswap can be used with disk swap but with higher priority.

## Undo

```
sudo swapoff /dev/vswap0
sudo vswap release
sudo rmmod vswap
```

> It is recommended to undo vswap before running a process that relies on VRAM (games, machine learning).

## Known limitations

- **Suspend and hibernation are refused:** while VRAM is attached, because VRAM loses power in S3. 
  Use `swapoff` and `vswap release` first;
- **A GPU reset can wipe VRAM:** processes with pages in VRAM would read garbage. 
  If `dmesg` shows a GPU reset, `swapoff` and reboot;
- **Read speed follows the GPU memory clock:** when the GPU idles at low memory clocks (screen off), a 4 KiB swap-in takes about 38 µs instead of 8 µs.
- **Tested only on amdgpu/RADV:** other Mesa drivers need checking that `dma_buf_vmap` pins in VRAM and that their mmap fault handler maps real frames.

## TODO

- [ ] GPU reset detection, if resets ever show up.
- [ ] Test other Mesa drivers (Intel, nouveau).
- [ ] Support more than one vswap device (`/dev/vswap0`, `/dev/vswap1`, ...).

## License

GPL-2.0. See [LICENSE](LICENSE).
