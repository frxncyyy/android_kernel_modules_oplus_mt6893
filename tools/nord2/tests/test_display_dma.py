#!/usr/bin/env python3
"""Check that PRIME imports use the scanout device, including SMMU remapping."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
driver = root / "kernel/kernel_device_modules-6.6/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_gem.c"
source = driver.read_text()
start = source.index("struct drm_gem_object *mtk_gem_prime_import(")
end = source.index("\nstruct drm_gem_object *\nmtk_gem_prime_import_sg_table", start)
harness = r'''
#include <assert.h>
#include <stddef.h>
#define MMSYS_MT6768 6768
#define WARN_ON(x) (x)
#define DDPINFO(...) do {} while (0)
struct device { int unused; } master, ovl, shared;
struct chip { int mmsys_id; } chip;
struct mtk_drm_private { struct chip *data; struct device *dma_dev; } private;
struct drm_device { struct device *dev; void *dev_private; } drm;
struct list { struct list *next, *prev; };
struct dma_buf { void *file; struct list attachments; } buf;
struct drm_gem_object { int unused; } gem;
static int smmu, imports;
static struct device *mtk_smmu_get_shared_device(struct device *dev)
{
    assert(dev==&ovl);
    return smmu ? &shared : dev;
}
static struct drm_gem_object *drm_gem_prime_import_dev(struct drm_device *dev,
        struct dma_buf *dma_buf, struct device *dma_dev)
{
    assert(dev==&drm && dma_buf==&buf);
    assert(dma_dev==(smmu ? &shared : &ovl));
    imports++;
    return &gem;
}
FUNCTION
int main(void)
{
    private.data=&chip; private.dma_dev=&ovl;
    drm.dev=&master; drm.dev_private=&private;
    buf.file=&buf; buf.attachments.next=buf.attachments.prev=&buf.attachments;
    for (smmu=0;smmu<2;smmu++) for (int legacy=0;legacy<2;legacy++) {
        chip.mmsys_id=legacy ? 6768 : 6885;
        assert(mtk_gem_prime_import(&drm,&buf)==&gem);
    }
    assert(imports==4);
}
'''.replace("FUNCTION", source[start:end])
with tempfile.TemporaryDirectory(prefix="nord2-display-dma-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: PRIME attaches to the OVL DMA domain or its shared SMMU device")
