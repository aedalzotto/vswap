# vswap design

vswap turns a slice of VRAM into a swap device that the kernel drives on its own. 
Allocation happens once, in userspace, through Vulkan.
Every swap I/O after that is a CPU copy inside the kernel.

## Why in the kernel

vramfs exposes VRAM as a FUSE filesystem and puts a swap file on it. 
Every swap-out then goes through a userspace daemon. 
Under heavy memory pressure the daemon (or something it depends on) needs memory or gets paged out itself, and the system stalls.
The swap path waits on a process that waits on the swap path. 
vswap keeps the whole I/O path in the kernel and allocates nothing in it.

## Allocation (`vswap attach <size>`)

1. Create a Vulkan device and allocate `size` in 1 GiB chunks from a `DEVICE_LOCAL | HOST_VISIBLE` memory type, exportable as a dma-buf. 
RADV caps one allocation at 4 GiB. 1 GiB chunks keep the module's offset math to a division.
2. `mmap` each dma-buf and write a tag word every MiB.
3. Destroy the Vulkan device. 
The dma-buf fds alone now keep the VRAM alive.
4. `VSWAP_ATTACH {fd, addr}` per chunk, where `addr` is the tool's mmap of it.
5. Read every tag back through `/dev/vswap0` with `O_DIRECT`. 
Any mismatch: `VSWAP_RELEASE` and fail.

The tool then exits. 

## Attaching a chunk (module)

1. `dma_buf_get(fd)`, then `dma_buf_vmap`.
The vmap is kept only because it **pins** the buffer where it is (amdgpu pins to GTT or VRAM on vmap).
`dma_buf_pin` is not used since amdgpu moves the buffer to system RAM for any importer that is not a peer-to-peer PCI device.
2. If the vmap is not `is_iomem`, the buffer is in system RAM. Refuse.
3. Walk the caller's mmap of the dma-buf with `follow_pfnmap_start` and collect the page frame of every page. 
The exporter's fault handler placed those, so they are right even when the buffer is fragmented in VRAM. 
A frame with a `struct page` means system RAM. Refuse.
4. `vmap_pfn(..., pgprot_writecombine(PAGE_KERNEL))`: one virtually contiguous, write-combined kernel mapping per chunk. 
All I/O goes through this mapping.
5. Create `/dev/vswap0` on the first chunk, grow its capacity on each one.

Why not copy through the dma-buf vmap: TTM's `ttm_bo_vmap` maps one physically contiguous range starting at the buffer's first block, without checking that the buffer is contiguous. 
For a fragmented buffer that aliases other VRAM. 
On amdgpu it also maps with the default `ttm_cached` caching, so the mapping is not the write-combined one reads need. 
Asking amdgpu for contiguous VRAM from userspace is not possible (`AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS` returns EINVAL).

## I/O path

- Bio-based, like `brd` and `zram`, with 4 KiB logical blocks and `BLK_FEAT_SYNCHRONOUS`. 
Swap-in then skips the swap cache and waits on our copy.
- Sector N lives at byte N×512 of the device; chunk = offset / chunk size. 
- **Write:** `memcpy_toio` into the WC mapping, then `wmb()` before completing the bio so the write-combining buffers are drained.
- **Read:** `drm_memcpy_from_wc`, which uses `movntdqa` streaming loads. 
Plain loads from WC memory are about 20× slower.
- A semaphore allows at most `max_readers` (default 2) concurrent readers.
BAR reads lose throughput beyond two (on AMD Navi 21).
- `REQ_OP_DISCARD` completes immediately. 
Other ops fail.
- No allocation, no GPU command, no lock shared with allocation.

## Release, unload, suspend

- `VSWAP_RELEASE` refuses with EBUSY while `/dev/vswap0` is open (swap on). 
Otherwise it deletes the disk and unmaps and drops every dma-buf.
- An open `/dev/vswap0` holds a module reference, so `rmmod` fails while swap is on.
- A PM notifier refuses suspend and hibernation while any chunk is attached.

## ioctl ABI (`/dev/vswap-ctl`, mode 0600)

| ioctl | Argument | Effect |
| --- | --- | --- |
| `VSWAP_ATTACH` | `struct vswap_attach { s32 fd; u32 pad; u64 addr; }` | Append one dma-buf; `addr` is the caller's `MAP_SHARED` mmap of all of it. |
| `VSWAP_RELEASE` | none | Drop the device and all VRAM. |

Limits: up to 64 chunks; every chunk must have the size of the first.

## Alternatives

A tmpfs-like VRAM filesystem such as (vramfs)[] was rejected due to generic files needing VRAM allocation at write time through a daemon.
Reads would be slower than an SSD for large files, and tmpfs backed by vswap swap covers the use case.
