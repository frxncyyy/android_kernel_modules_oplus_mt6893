#!/usr/bin/env python3
"""Inject DMA-BUF acquisition failures and partial GUP into actual TEE code."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "kernel/kernel_device_modules-6.6/drivers/tee/gud/500/MobiCoreDriver/mmu.c"
text = source.read_text()
start = text.rfind("\nstatic ", 0, text.index("mmu_get_dma_buffer(")) + 1
acquire = text[start:text.index("\n}", start) + 2]
start = text.index("\tif (mmu->dma_buf) {", text.index("static void tee_mmu_delete("))
cleanup = text[start:text.index("\n#endif", start)]
start = text.index("\t\t\tif (gup_ret != nr_pages)")
partial = text[start:text.index("\n\n", start)]
start = text.index("static void tee_mmu_unpin_page(")
unpin = text[start:text.index("\n}", start) + 2]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define CONFIG_DMA_SHARED_BUFFER 1
#define PAGE_SIZE 4096
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define DMA_BIDIRECTIONAL 0
#define MC_IO_MAP_OUTPUT 2
#define IS_ERR(p) ((intptr_t)(p)<0 && (intptr_t)(p)>-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define mc_dev_err(...) do {} while (0)
struct dma_buf { size_t size; } buffer;
struct dma_buf_attachment { int unused; } attachment;
struct sg_table { int unused; } table;
struct tee_mmu {
    struct dma_buf *dma_buf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
    size_t length, nr_pages;
    unsigned flags;
};
static struct { void *mcd; } g_ctx;
static int fail, refs, attaches, maps;
static struct dma_buf *dma_buf_get(int fd) {
    assert(fd==7);
    if (fail==1) return ERR_PTR(-EBADF);
    refs++; return &buffer;
}
static void dma_buf_put(struct dma_buf *buf) {
    assert(buf==&buffer && refs==1 && attaches==0); refs--;
}
static struct dma_buf_attachment *dma_buf_attach(struct dma_buf *buf, void *dev) {
    (void)dev; assert(buf==&buffer && refs==1);
    if (fail==2) return ERR_PTR(-ENOMEM);
    attaches++; return &attachment;
}
static void dma_buf_detach(struct dma_buf *buf, struct dma_buf_attachment *attach) {
    assert(buf==&buffer && attach==&attachment && attaches==1 && maps==0);
    attaches--;
}
static struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *attach, int direction) {
    assert(attach==&attachment && attaches==1 && direction==DMA_BIDIRECTIONAL);
    if (fail==3) return ERR_PTR(-EIO);
    maps++; return &table;
}
static void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *attach, struct sg_table *sgt, int direction) {
    assert(attach==&attachment && sgt==&table && maps==1 && direction==DMA_BIDIRECTIONAL);
    maps--;
}
/* A caller without the reservation lock must use the unlocked wrappers. */
static __attribute__((unused)) struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *a, int d) {
    (void)a; (void)d; assert(!"reservation lock missing"); return NULL;
}
static __attribute__((unused)) void dma_buf_unmap_attachment(struct dma_buf_attachment *a, struct sg_table *s, int d) {
    (void)a; (void)s; (void)d; assert(!"reservation lock missing");
}
ACQUIRE
static void cleanup(struct tee_mmu *mmu) {
CLEANUP
}
struct page { int pins, refs; bool dirty; };
static void unpin_user_pages(struct page **pages, unsigned long n) {
    for (unsigned long i=0;i<n;i++) { assert(pages[i]->pins==1); pages[i]->pins--; }
}
static __attribute__((unused)) void release_pages(struct page **pages, unsigned long n) {
    for (unsigned long i=0;i<n;i++) pages[i]->refs--;
}
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long n, bool dirty) {
    for (unsigned long i=0;i<n;i++) if (dirty) pages[i]->dirty=true;
    unpin_user_pages(pages,n);
}
UNPIN
static int partial_pin(long gup_ret, unsigned long nr_pages, struct page **pages) {
    int ret=0;
PARTIAL
end:
    return ret;
}
int main(void) {
    const int errors[]={0,-EBADF,-ENOMEM,-EIO};
    for (fail=0;fail<4;fail++) {
        struct tee_mmu mmu={.length=4096,.nr_pages=1}; buffer.size=4096;
        refs=attaches=maps=0;
        assert(mmu_get_dma_buffer(&mmu,7)==errors[fail]);
        if (fail) assert(!mmu.dma_buf && !mmu.attach && !mmu.sgt);
        else assert(mmu.dma_buf && mmu.attach && mmu.sgt);
        cleanup(&mmu);
        assert(refs==0 && attaches==0 && maps==0);
    }
    fail=0;
    for (int which=0;which<2;which++) {
        struct tee_mmu mmu={.length=which ? 4096 : 4097,.nr_pages=which ? 2 : 1};
        buffer.size=4096; refs=attaches=maps=0;
        assert(mmu_get_dma_buffer(&mmu,7)==-EINVAL);
        assert(!mmu.dma_buf && !mmu.attach && !mmu.sgt);
        cleanup(&mmu); assert(refs==0 && attaches==0 && maps==0);
    }
    for (unsigned pinned=0;pinned<=4;pinned++) {
        struct page storage[4]; struct page *pages[4];
        for (unsigned i=0;i<4;i++) { storage[i]=(struct page){.pins=i<pinned,.refs=1}; pages[i]=&storage[i]; }
        assert(partial_pin(pinned,4,pages)==(pinned==4 ? 0 : -EINVAL));
        for (unsigned i=0;i<4;i++) {
            assert(storage[i].refs==1);
            assert(storage[i].pins==(pinned==4 ? 1 : 0));
        }
    }
    for (unsigned flags=1;flags<=3;flags++) {
        struct tee_mmu mmu={.flags=flags};
        struct page page={.pins=1,.refs=1,.dirty=false};
        tee_mmu_unpin_page(&mmu,&page);
        assert(page.pins==0 && page.refs==1);
        assert(page.dirty==!!(flags & MC_IO_MAP_OUTPUT));
    }
}
'''.replace("ACQUIRE", acquire).replace("CLEANUP", cleanup).replace("PARTIAL", partial).replace("UNPIN", unpin)
with tempfile.TemporaryDirectory(prefix="nord2-tee-memory-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: DMA-BUF failures unwind once, size checks hold, partial pins release correctly, output pages are dirtied")
