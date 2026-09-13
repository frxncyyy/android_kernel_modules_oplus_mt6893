// SPDX-License-Identifier: MIT
#include "nolibc.h"
#include <linux/ioctl.h>
#include <linux/dma-heap.h>
#include "mc_user.h"
#define TEE_MEM_INPUT 1
#define TEE_MEM_OUTPUT 2
#define TEE_MEM_DMABUF (1u << 24)
static int cycle(int fd, __u64 address, size_t size, unsigned flags)
{
    struct mc_ioctl_gp_register_shared_mem reg={.memref={.buffer=address,.size=size,.flags=flags}};
    if (ioctl(fd, MC_IO_GP_REGISTER_SHARED_MEM, &reg) || reg.ret.value) {
        printf("Registration failed: errno=%d return=0x%x origin=%u size=%lu flags=0x%x\n",
               errno,reg.ret.value,reg.ret.origin,(unsigned long)size,flags); return 1;
    }
    struct mc_ioctl_gp_release_shared_mem release={.memref=reg.memref};
    if (ioctl(fd, MC_IO_GP_RELEASE_SHARED_MEM, &release)) { perror("release"); return 1; }
    return 0;
}
int main(int argc, char **argv)
{
    int fd=open("/dev/mobicore-user",O_RDWR|O_CLOEXEC);
    if (fd<0) { perror("TEE open"); return 1; }
    struct mc_version_info version={0};
    if (ioctl(fd,MC_IO_VERSION,&version) || version.version_mci!=0x10008 || version.version_nwd!=0x80003) {
        puts("This probe requires MCI 1.8 and NWD ABI 8.3"); close(fd); return 1;
    }
    struct mc_ioctl_gp_initialize_context context={0};
    if (ioctl(fd,MC_IO_GP_INITIALIZE_CONTEXT,&context) || context.ret.value) {
        puts("Context initialization failed"); close(fd); return 1;
    }
    const size_t sizes[]={4096,3*4096+17,2*1024*1024+4096};
    for (unsigned repeat=0;repeat<4;repeat++) for (unsigned i=0;i<3;i++) {
        size_t size=sizes[i];
        char *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (p==MAP_FAILED) { perror("mmap"); close(fd); return 1; }
        memset(p,0x69,size);
        int ret=cycle(fd,(__u64)p,size,TEE_MEM_INPUT|TEE_MEM_OUTPUT);
        munmap(p,size);
        if (ret) { close(fd); return ret; }
    }
    puts("12 anonymous read/write shared-memory cycles passed, including multi-table buffers");
    char *ro=mmap(NULL,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (ro==MAP_FAILED || cycle(fd,(__u64)ro,4096,TEE_MEM_INPUT)) { close(fd); return 1; }
    munmap(ro,4096); puts("Read-only input registration passed");
    if (argc>1) {
        int heap=open(argv[1],O_RDWR|O_CLOEXEC);
        if (heap<0) { perror("DMA heap open"); close(fd); return 1; }
        for (unsigned i=0;i<3;i++) {
            struct dma_heap_allocation_data alloc={.len=sizes[i],.fd_flags=O_RDWR|O_CLOEXEC};
            if (ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&alloc)) { perror("heap alloc"); close(heap); close(fd); return 1; }
            int ret=cycle(fd,alloc.fd,sizes[i],TEE_MEM_INPUT|TEE_MEM_OUTPUT|TEE_MEM_DMABUF);
            close(alloc.fd);
            if (ret) { close(heap); close(fd); return ret; }
        }
        close(heap); puts("Three DMA-BUF shared-memory cycles passed");
    }
    return close(fd)!=0;
}
