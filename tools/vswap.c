#define _GNU_SOURCE  // O_DIRECT
// vswap: allocates VRAM through Vulkan and hands it to vswap.ko as dma-bufs.
// Runs only at setup and exits.
//   vswap attach <size>   attach VRAM to /dev/vswap0 (needs vswap.ko); size like 12G or 12288M
//   vswap release         give the VRAM back (swapoff first)
//   vswap test <size>     no module: check the buffers outlive this program's Vulkan device
// build: make tool (or gcc -O2 -Wall -I.. vswap.c -lvulkan -o vswap)
#include <vulkan/vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "vswap_uapi.h"

#define CHUNK (1ull << 30)
#define STEP (1ull << 20)

#define CK(x)								\
	do {								\
		VkResult r_ = (x);					\
		if (r_) {						\
			fprintf(stderr, "%s failed: %d\n", #x, r_);	\
			exit(1);					\
		}							\
	} while (0)

static VkInstance in;
static VkDevice dev;
static int vswap;

static int pick_memory(VkPhysicalDevice gpu)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu, &mp);

	// DEVICE_LOCAL|HOST_VISIBLE = VRAM the CPU can reach over the BAR; uncached, so not the GTT type
	VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if (
			   ((f & want) == want)
			&& !(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
			&& (mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size > CHUNK)
		)
			return i;
	}

	fprintf(stderr, "no large DEVICE_LOCAL|HOST_VISIBLE memory: is Resizable BAR on?\n");
	exit(1);
}

// Allocates n chunks of VRAM and returns their dma-buf fds in fds[].
static void alloc_chunks(int n, int *fds, VkDeviceMemory *mem)
{
	CK(vkCreateInstance(
		&(VkInstanceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &(VkApplicationInfo){
				.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				.apiVersion = VK_API_VERSION_1_1
			}
		},
		0,
		&in
	));

	VkPhysicalDevice pd[8];
	VkPhysicalDevice gpu = 0;
	uint32_t np = 8;

	CK(vkEnumeratePhysicalDevices(in, &np, pd));
	for (uint32_t i = 0; i < np && !gpu; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pd[i], &p);
		if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			gpu = pd[i];
			fprintf(stderr, "GPU: %s\n", p.deviceName);
		}
	}

	if (!gpu) {
		fprintf(stderr, "no discrete GPU\n");
		exit(1);
	}

	int mt = pick_memory(gpu);

	const char *ext[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME
	};
	float prio = 1;

	CK(vkCreateDevice(
		gpu,
		&(VkDeviceCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueCount = 1,
				.pQueuePriorities = &prio
			},
			.enabledExtensionCount = 2,
			.ppEnabledExtensionNames = ext
		},
		0,
		&dev
	));

	PFN_vkGetMemoryFdKHR getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");

	for (int i = 0; i < n; i++) {
		VkExportMemoryAllocateInfo exp = {
			.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
		};

		VkResult r = vkAllocateMemory(
			dev,
			&(VkMemoryAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.pNext = &exp,
				.allocationSize = CHUNK,
				.memoryTypeIndex = mt
			},
			0,
			&mem[i]
		);
		if (r) {
			fprintf(stderr, "out of VRAM after %d MiB (%d)\n", i * (int)(CHUNK >> 20), r);
			exit(1);
		}

		CK(getfd(
			dev,
			&(VkMemoryGetFdInfoKHR){
				.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
				.memory = mem[i],
				.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
			},
			&fds[i]
		));
	}
}

// Drops every Vulkan object; only the dma-buf fds keep the VRAM alive after this.
static void close_vulkan(int n, VkDeviceMemory *mem)
{
	for (int i = 0; i < n; i++)
		vkFreeMemory(dev, mem[i], 0);

	vkDestroyDevice(dev, 0);
	vkDestroyInstance(in, 0);
}

static uint64_t tag(int chunk, size_t off)
{
	return (uint64_t)chunk << 40 ^ off ^ 0x6b7672616d6673ull;
}

static volatile uint64_t *map_chunk(int fd)
{
	volatile uint64_t *p = mmap(0, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap dma-buf");
		exit(1);
	}

	return p;
}

// Reads one word at byte off of chunk i through /dev/vswap0, i.e. through the module's mapping.
static uint64_t read_vswap(int i, size_t off)
{
	static uint64_t *page;
	if (!page && posix_memalign((void **)&page, 4096, 4096))
		exit(1);

	if (pread(vswap, page, 4096, (off_t)i * CHUNK + off) != 4096) {
		perror("read /dev/vswap0");
		exit(1);
	}

	return page[0];
}

// "12G" or "12288M" (a bare number is MiB) -> MiB, or -1
static long parse_mib(const char *s)
{
	char *end;

	long v = strtol(s, &end, 10);
	if (v <= 0)
		return -1;

	if (!strcasecmp(end, "G"))
		return v * 1024;

	if (!*end || !strcasecmp(end, "M"))
		return v;

	return -1;
}

static int usage(const char *argv0)
{
	fprintf(
		stderr,
		"usage: %s attach <size> | release | test <size>\n"
		"  size: 12G or 12288M, a multiple of %llu MiB\n",
		argv0,
		CHUNK >> 20
	);
	return 1;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	if (!strcmp(cmd, "release") && argc == 2) {
		int ctl = open("/dev/vswap-ctl", O_RDWR);
		if (ctl < 0 || ioctl(ctl, VSWAP_RELEASE)) {
			perror("release");
			return 1;
		}
		return 0;
	}

	int test = !strcmp(cmd, "test");
	if ((!test && strcmp(cmd, "attach")) || argc != 3)
		return usage(argv[0]);

	long mib = parse_mib(argv[2]);
	if (mib <= 0 || mib % (CHUNK >> 20))
		return usage(argv[0]);

	int n = mib / (CHUNK >> 20), ctl = -1, fds[n];
	long bad = 0;
	VkDeviceMemory mem[n];
	volatile uint64_t *maps[n];

	// open the module first so a missing module fails before we take the VRAM
	if (!test && (ctl = open("/dev/vswap-ctl", O_RDWR)) < 0) {
		perror("/dev/vswap-ctl (is vswap.ko loaded?)");
		return 1;
	}

	alloc_chunks(n, fds, mem);
	for (int i = 0; i < n; i++) {
		maps[i] = map_chunk(fds[i]);
		for (size_t o = 0; o < CHUNK; o += STEP)
			maps[i][o / 8] = tag(i, o);
	}
	close_vulkan(n, mem);  // from here on only the dma-bufs hold the VRAM

	if (test) {
		for (int i = 0; i < n; i++) {
			for (size_t o = 0; o < CHUNK; o += STEP)
				bad += maps[i][o / 8] != tag(i, o);
		}
		printf(
			"%ld MiB in %d dma-bufs, Vulkan closed, %ld/%llu words wrong: %s\n",
			mib, n, bad, n * (CHUNK / STEP), bad ? "FAIL" : "PASS"
		);
		return !!bad;
	}

	for (int i = 0; i < n; i++) {
		struct vswap_attach a = {
			.fd = fds[i],
			.addr = (uintptr_t)maps[i]
		};
		if (ioctl(ctl, VSWAP_ATTACH, &a)) {
			if (errno == EMEDIUMTYPE)  // the driver put this chunk in system RAM: VRAM is full
				fprintf(stderr, "chunk %d of %d is not in VRAM: ask for less (released)\n", i + 1, n);
			else
				perror("VSWAP_ATTACH (see dmesg)");
			ioctl(ctl, VSWAP_RELEASE);
			return 1;
		}
	}

	for (int t = 0; (vswap = open("/dev/vswap0", O_RDONLY | O_DIRECT)) < 0 && t < 50; t++)
		usleep(100000);  // udev creates the node asynchronously

	if (vswap < 0) {
		perror("/dev/vswap0");
		ioctl(ctl, VSWAP_RELEASE);
		return 1;
	}

	for (int i = 0; i < n; i++) {
		for (size_t o = 0; o < CHUNK; o += STEP)
			bad += read_vswap(i, o) != tag(i, o);
	}

	close(vswap);

	if (bad) {  // the module sees different memory than we wrote: never swap onto that
		fprintf(
			stderr,
			"/dev/vswap0 read back %ld/%llu wrong words: mapping not usable, released\n",
			bad, n * (CHUNK / STEP)
		);
		ioctl(ctl, VSWAP_RELEASE);
		return 1;
	}

	printf("%ld MiB attached and verified through /dev/vswap0\n", mib);
	return 0;
}
