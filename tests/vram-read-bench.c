// Phase 0: how fast can the CPU read VRAM over the BAR? No root needed.
// Allocates DEVICE_LOCAL|HOST_VISIBLE memory through Vulkan (VRAM via ReBAR,
// mapped write-combined by Mesa), then times random 4 KiB copies to RAM.
// build: gcc -O2 -mavx2 -pthread vram-read-bench.c -lvulkan -o vram-read-bench
#include <vulkan/vulkan.h>
#include <immintrin.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SZ (1ull << 30)  // 1 GiB test window
#define PG 4096
#define SECS 2.0

static char *vram;
static atomic_int stop;
typedef void (*copyfn)(void *, const void *);

static void copy_memcpy(void *d, const void *s) { memcpy(d, s, PG); }
static void copy_ntload(void *d, const void *s)  // movntdqa: 64 B per WC read burst
{
	__m128i *dd = d, *ss = (__m128i *)s;
	for (int i = 0; i < PG / 16; i += 4) {
		__m128i a = _mm_stream_load_si128(ss + i), b = _mm_stream_load_si128(ss + i + 1);
		__m128i c = _mm_stream_load_si128(ss + i + 2), e = _mm_stream_load_si128(ss + i + 3);
		_mm_store_si128(dd + i, a); _mm_store_si128(dd + i + 1, b);
		_mm_store_si128(dd + i + 2, c); _mm_store_si128(dd + i + 3, e);
	}
}

static void copy_ntload256(void *d, const void *s)  // vmovntdqa ymm: 32 B loads
{
	__m256i *dd = d, *ss = (__m256i *)s;
	for (int i = 0; i < PG / 32; i += 4) {
		__m256i a = _mm256_stream_load_si256(ss + i), b = _mm256_stream_load_si256(ss + i + 1);
		__m256i c = _mm256_stream_load_si256(ss + i + 2), e = _mm256_stream_load_si256(ss + i + 3);
		_mm256_store_si256(dd + i, a); _mm256_store_si256(dd + i + 1, b);
		_mm256_store_si256(dd + i + 2, c); _mm256_store_si256(dd + i + 3, e);
	}
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

struct job { copyfn f; unsigned seed; long pages; };
static void *worker(void *p)
{
	struct job *j = p;
	char *buf = aligned_alloc(64, PG);
	while (!atomic_load(&stop)) {
		j->f(buf, vram + (size_t)(rand_r(&j->seed) % (SZ / PG)) * PG);
		j->pages++;
	}
	free(buf);
	return NULL;
}

static double run(copyfn f, int nt)
{
	pthread_t th[64]; struct job j[64];
	atomic_store(&stop, 0);
	for (int i = 0; i < nt; i++) { j[i] = (struct job){ f, i + 1, 0 }; pthread_create(&th[i], 0, worker, &j[i]); }
	double t = now(); usleep(SECS * 1e6); atomic_store(&stop, 1);
	long n = 0;
	for (int i = 0; i < nt; i++) { pthread_join(th[i], 0); n += j[i].pages; }
	return n * (double)PG / (now() - t) / (1 << 20);
}

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s = %d\n", #x, r_); exit(1); } } while (0)

int main(void)
{
	VkInstance in; VkPhysicalDevice pd[8], gpu = 0; uint32_t n = 8;
	CK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 } }, 0, &in));
	CK(vkEnumeratePhysicalDevices(in, &n, pd));
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i], &p);
		if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = pd[i]; printf("GPU: %s\n", p.deviceName); break; }
	}
	if (!gpu) { fprintf(stderr, "no discrete GPU\n"); return 1; }

	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
	VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	int mt = -1;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
		    && mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size > SZ) { mt = i; break; }
	if (mt < 0) { fprintf(stderr, "no large DEVICE_LOCAL|HOST_VISIBLE heap: ReBAR off?\n"); return 1; }
	printf("memory type %d, heap %u: %llu MiB\n", mt, mp.memoryTypes[mt].heapIndex,
	       (unsigned long long)mp.memoryHeaps[mp.memoryTypes[mt].heapIndex].size >> 20);

	VkDevice dev; float prio = 1;
	CK(vkCreateDevice(gpu, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio } }, 0, &dev));
	VkDeviceMemory mem;
	CK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = SZ, .memoryTypeIndex = mt }, 0, &mem));
	CK(vkMapMemory(dev, mem, 0, SZ, 0, (void **)&vram));

	// write = what swap-out will do; also gives the read test real data to check
	char *ram = malloc(SZ);
	for (size_t i = 0; i < SZ / 8; i++) ((unsigned long *)ram)[i] = i * 0x9e3779b97f4a7c15ul;
	double t = now(); memcpy(vram, ram, SZ); _mm_sfence();
	printf("sequential write 1 GiB: %.0f MiB/s\n", 1024 / (now() - t));

	char buf[PG] __attribute__((aligned(64)));
	for (size_t o = 0; o < SZ; o += SZ / 64) {  // spot-check both copy paths
		copy_ntload(buf, vram + o); if (memcmp(buf, ram + o, PG)) { puts("MISMATCH (movntdqa)"); return 1; }
		copy_memcpy(buf, vram + o); if (memcmp(buf, ram + o, PG)) { puts("MISMATCH (memcpy)"); return 1; }
	}
	puts("data check: ok\n\nrandom 4 KiB reads, MiB/s (SSD baseline: 957)");
	printf("%-8s %10s %10s %10s\n", "threads", "memcpy", "movntdqa", "ymm");
	int ts[] = { 1, 2, 4, 8, 16 };
	for (int i = 0; i < 5; i++)
		printf("%-8d %10.1f %10.1f %10.1f\n", ts[i], run(copy_memcpy, ts[i]), run(copy_ntload, ts[i]), run(copy_ntload256, ts[i]));
	return 0;
}
