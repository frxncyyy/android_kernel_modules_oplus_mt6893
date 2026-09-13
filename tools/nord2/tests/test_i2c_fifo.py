#!/usr/bin/env python3
"""Exercise the real reset and transfer-selection blocks without DMA hardware."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = Path(__file__).resolve().parents[3] / 'kernel/kernel_device_modules-6.6/drivers/i2c/busses/i2c-mt65xx.c'
text = source.read_text()
a = text.index('\tif (i2c->fifo_only) {', text.index('static void mtk_i2c_init_hw'))
b = text.index('\n\t/* config scp', a)
reset = text[a:b]
a = text.index('\tif ((msgs->len > i2c->dev_comp->fifo_size)')
b = text.index('\n\t/* Make sure the clock', a)
select = text[a:b]
constants = sorted(set(re.findall(r'\b(?:(?:I2C|OFFSET)_\w+|DMA_HW_VERSION\d+)', reset + select)))
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
ENUMS
struct compat { int fifo_size, apdma_sync, dma_ver; };
struct controller { bool fifo_only; char *pdmabase; struct compat *dev_comp; int op, ch_offset_i2c, i2c_offset_scp; };
struct msg { int len; void *buf; };
static unsigned dma_writes, controller_writes, frees;
static void writel(int val, void *ptr) { (void)val; assert(ptr); dma_writes++; }
static void mtk_i2c_writew(struct controller *c, int val, int reg) { (void)c; (void)val; (void)reg; controller_writes++; }
static void udelay(int x) { (void)x; }
static void mb(void) {}
static void kfree(void *p) { (void)p; frees++; }
#define dev_dbg(...) do {} while (0)
static void reset(struct controller *i2c)
{
@RESET@
}
static int choose(struct controller *i2c, struct msg *msgs)
{
    bool isDMA;
@SELECT@
    return isDMA;
}
int main(void)
{
    struct compat caps = { .fifo_size=8, .apdma_sync=1, .dma_ver=DMA_HW_VERSION1 };
    struct controller c = { .fifo_only=true, .pdmabase=NULL, .dev_comp=&caps, .ch_offset_i2c=0, .i2c_offset_scp=0x100 };
    struct msg msgs[2] = {{1,NULL},{2,NULL}};
    reset(&c);
    assert(dma_writes==0 && controller_writes==1);
    c.op=I2C_MASTER_WRRD;
    for (int wr=1; wr<=9; wr++) for (int rd=1; rd<=9; rd++) {
        msgs[0].len=wr; msgs[1].len=rd;
        assert(choose(&c,msgs)==((wr>8 || rd>8) ? -EOPNOTSUPP : 0));
    }
    c.op=I2C_MASTER_CONTINUOUS_WR; msgs[0].len=9;
    assert(choose(&c,msgs)==-EOPNOTSUPP && frees==1);
    c.fifo_only=false; c.pdmabase=(char *)(uintptr_t)0x1000;
    reset(&c); assert(dma_writes>0);
    assert(choose(&c,msgs)==1);
    c.ch_offset_i2c=c.i2c_offset_scp;
    assert(choose(&c,msgs)==-EPERM && frees==2);
}
'''.replace('ENUMS','enum {'+','.join(constants)+'};').replace('@RESET@',reset).replace('@SELECT@',select)
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory);(d/'test.c').write_text(harness)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
print('PASS: FIFO reset avoids DMA; 1..8-byte combined transfers pass; oversize/coalesced writes reject; DMA/SCP behavior retained')
