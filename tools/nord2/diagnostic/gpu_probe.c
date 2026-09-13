// SPDX-License-Identifier: MIT
#include "nolibc.h"
#include <gpu/arm/midgard/mali_kbase_ioctl.h>
typedef __u32 base_mem_alloc_flags;
#define LOCAL_PAGE_SHIFT 12
#include <gpu/arm/midgard/mali_base_common_kernel.h>
static int memory_test(int fd)
{
    if (getauxval(AT_PAGESZ) != 4096) { puts("This test requires 4 KiB pages"); return 1; }
    void *tracking = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE);
    if (tracking == MAP_FAILED) { perror("tracking mmap"); return 1; }
    int result = 1;
    for (unsigned round = 0; round < 32; round++) {
        unsigned pages = 1u << (2 * (round % 5));
        size_t bytes = pages * 4096;
        union kbase_ioctl_mem_alloc mem = {.in = {
            .va_pages = pages, .commit_pages = pages,
            .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA}};
        if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &mem)) { perror("memory alloc"); goto out; }
        volatile __u32 *cpu = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mem.out.gpu_va);
        if (cpu == MAP_FAILED) { perror("allocation mmap"); goto out; }
        union kbase_ioctl_mem_query query = {.in = {.gpu_addr = (__u64)cpu, .query = KBASE_MEM_QUERY_COMMIT_SIZE}};
        if (ioctl(fd, KBASE_IOCTL_MEM_QUERY, &query) || query.out.value != pages) {
            perror("memory query"); munmap((void *)cpu, bytes); goto out;
        }
        for (unsigned i=0;i<bytes/4;i++) cpu[i] = 0x68930000u ^ (round * 65537u) ^ i;
        for (unsigned i=0;i<bytes/4;i++) {
            if (cpu[i] != (0x68930000u ^ (round * 65537u) ^ i)) {
                puts("CPU mapped-buffer readback mismatch"); munmap((void *)cpu, bytes); goto out;
            }
        }
        /* Unmapping a SAME_VA allocation also releases the GPU region. */
        if (munmap((void *)cpu, bytes)) { perror("allocation munmap"); goto out; }
    }
    puts("32 GPU buffer allocate/map/query/CPU-readback/unmap cycles passed (4 KiB to 1 MiB)");
    result = 0;
out:
    munmap(tracking, 4096);
    return result;
}
static unsigned char props[8192];
int main(void)
{
    for (unsigned minor = 0; minor <= 46; minor += 23) {
        int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
        if (fd < 0) { perror("open mali0"); return 1; }
        struct kbase_ioctl_version_check version = {.major = 11, .minor = minor};
        if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &version)) { perror("version"); close(fd); return 1; }
        printf("Requested ABI 11.%u, negotiated %u.%u\n", minor, version.major, version.minor);
        if (version.major != 11 || version.minor > minor) {
            puts("Unexpected negotiated ABI"); close(fd); return 1;
        }
        struct kbase_ioctl_get_gpuprops get = {0};
        int size = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &get);
        printf("GPU properties required bytes=%d\n", size);
        if (size <= 0 || size > sizeof(props)) { perror("properties size"); close(fd); return 1; }
        get.buffer = (__u64)props; get.size = sizeof(props);
        int got = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &get);
        if (got != size) { perror("properties read"); close(fd); return 1; }
        for (unsigned off = 0; off < (unsigned)size; ) {
            if (size - off < 4) return 2;
            unsigned key = 0; for (unsigned i=0;i<4;i++) key |= (unsigned)props[off+i] << (8*i);
            off += 4; unsigned bytes = 1u << (key & 3); __u64 value = 0;
            if (size - off < bytes) return 2;
            for (unsigned i=0;i<bytes;i++) value |= (__u64)props[off+i] << (8*i);
            off += bytes;
            if (!minor) printf("property %u: 0x%llx\n", key >> 2, (unsigned long long)value);
        }
        struct kbase_ioctl_set_flags flags = {0};
        if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &flags)) { perror("create context"); close(fd); return 1; }
        puts("Context created");
        if (memory_test(fd)) { close(fd); return 1; }
        close(fd); puts("Context closed");
    }
    return 0;
}
