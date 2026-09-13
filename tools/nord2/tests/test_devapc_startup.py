#!/usr/bin/env python3
"""Run the actual DEVAPC dump and ISR against latched-violation fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "kernel/kernel_device_modules-6.6/drivers/soc/mediatek/devapc/devapc-mtk-multi-ao.c"
text = source.read_text()


def function(signature):
    start = text.index(signature)
    end = text.index("\n}", start) + 2
    return text[start:end]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PFX "devapc: "
#define IRQ_TYPE_NUM_DEFAULT 1
#define IRQ_HANDLED 1
typedef int irqreturn_t;
enum devapc_vio_type { DEVAPC_VIO_DENIED, DEVAPC_VIO_ABNORMAL=4,
    DEVAPC_VIO_NO_VIO_FOUND };
struct mtk_device_num { int devapc_type; };
struct mtk_device_info { int sys_index, ctrl_index, vio_index; const char *device; };
struct mtk_devapc_vio_info { int domain_id, master_id, vio_addr, shift_sta_bit; };
struct soc {
    uint32_t slave_type_num, irq_type_num;
    bool boot_vio_report_only;
    const struct mtk_device_num *ndevices;
    const struct mtk_device_info **device_info;
    struct mtk_devapc_vio_info *vio_info;
    const char *(*master_get)(int, int, int, int, int);
};
struct context { struct soc *soc; int devapc_irq[1], current_irq_type; } ctx;
static struct context *mtk_devapc_ctx=&ctx;
static bool pending[3], masked[3], type2, null_master;
static int callbacks, clears, reports, inherited_reports;
static void log_message(const char *fmt, ...) {
    if (strstr(fmt, "%s %s %s %s\n")) reports++;
    if (strstr(fmt, "reported inherited violation")) inherited_reports++;
}
#define pr_info(...) log_message(__VA_ARGS__)
#define pr_warn(...) log_message(__VA_ARGS__)
#define smp_mb() do {} while (0)
#define spin_lock_irqsave(lock, flags) do { (flags)=0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); } while (0)
#define BUG_ON(condition) assert(!(condition))
static bool is_devapc_subsys_enabled(int type) { return type==0; }
static bool is_devapc_subsys_power_on(int type) { return type==0; }
static void set_devapc_subsys_power_off(int type) { assert(type==0); }
static bool get_violation(int bank, int *vio, int *index) {
    if (!pending[bank]) return false;
    *vio=419; *index=0; return true;
}
static bool check_type2_vio_status(int bank, int *vio, int *index) {
    return type2 && get_violation(bank, vio, index);
}
static bool mtk_devapc_dump_vio_dbg(int bank, int *vio, int *index) {
    return !type2 && get_violation(bank, vio, index);
}
static void mask_module_irq(int bank, int vio, bool mask) {
    assert(vio==419); masked[bank]=mask;
}
static int clear_vio_status(int bank, int vio) {
    assert(vio==419 && pending[bank] && masked[bank]);
    pending[bank]=false; clears++; return 0;
}
static uint8_t get_permission(int bank, int index, int domain) {
    (void)bank; assert(index==0 && domain==0); return 255;
}
static enum devapc_vio_type devapc_vio_reason(uint8_t perm) {
    assert(perm==255); return DEVAPC_VIO_DENIED;
}
static const char *master_get(int master, int addr, int bank, int shift, int domain) {
    (void)bank; (void)shift; assert(master==0x102 && addr==4 && domain==0);
    return null_master ? NULL : "APMCU_read";
}
static void devapc_extra_handler(int bank, const char *master,
    uint32_t vio, uint32_t addr, enum devapc_vio_type type) {
    assert(masked[bank] && !pending[bank] && vio==419 && addr==4);
    assert(master && type==DEVAPC_VIO_DENIED); callbacks++;
}
static bool is_matched_slave_type(int bank) { (void)bank; return true; }
static bool check_exception_vio_status(int type, int bank) {
    (void)type; (void)bank; return false;
}
static void print_slave_vio_mask_sta(int bank) { (void)bank; }
DUMP_FUNCTION
IRQ_FUNCTION
static void reset(void) {
    memset(pending, 0, sizeof(pending)); memset(masked, 0, sizeof(masked));
    callbacks=clears=reports=inherited_reports=0;
}
int main(void) {
    const struct mtk_device_num banks[3]={{0}, {0}, {0}};
    const struct mtk_device_info device={-1,-1,419,"SRAMROM"};
    const struct mtk_device_info *devices[3]={&device,&device,&device};
    struct mtk_devapc_vio_info violation={.master_id=0x102,.vio_addr=4};
    struct soc soc={.slave_type_num=3,.ndevices=banks,.device_info=devices,
        .vio_info=&violation,.master_get=master_get};
    ctx.soc=&soc; ctx.devapc_irq[0]=335;
    for (int policy=0;policy<2;policy++) {
        soc.boot_vio_report_only=policy;
        for (int t=0;t<2;t++) for (int unknown=0;unknown<2;unknown++) {
            type2=t; null_master=unknown;
            for (int boot=0;boot<2;boot++) {
                reset(); pending[0]=pending[1]=pending[2]=true;
                devapc_dump_info(boot);
                assert(clears==3 && reports==3);
                assert(callbacks==((policy && boot) ? 0 : 3));
                assert(inherited_reports==((policy && boot) ? 3 : 0));
                for (int b=0;b<3;b++) assert(!masked[b] && !pending[b]);
                devapc_dump_info(boot); /* Acknowledged status must stay clear. */
                assert(clears==3);
            }
            for (int b=0;b<3;b++) {
                reset(); pending[b]=true;
                assert(devapc_violation_irq(335,NULL)==IRQ_HANDLED);
                assert(callbacks==1 && clears==1 && reports==1);
                assert(!pending[b] && !masked[b]);
                assert(inherited_reports==0);
            }
            reset(); devapc_dump_info(true);
            assert(clears==0 && callbacks==0);
        }
    }
}
'''.replace("DUMP_FUNCTION", function("static void devapc_dump_info(bool booting)"))
harness = harness.replace("IRQ_FUNCTION", function("static irqreturn_t devapc_violation_irq("))

# Check the actual request loop with failure injection and delayed enablement.
probe = function("int mtk_devapc_probe(")
start = probe.rindex("\n\tfor (irq_type =", 0, probe.index("ret = devm_request_irq"))
request = probe[start:probe.index("\n\t/* CCF", start)]
finish = probe[probe.index("\n\tdevapc_dump_info(true);"):]
ordering = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define IRQF_TRIGGER_NONE 0
#define IRQF_NO_AUTOEN 0x80000
#define PFX ""
#define pr_err(...) do {} while (0)
typedef void (*irq_handler_t)(void);
struct platform_device { int dev; } device;
struct context { int devapc_irq[2]; } context={{335,336}}, *mtk_devapc_ctx=&context;
static int requested, enabled, fail_request;
static bool dumped, started;
static void devapc_violation_irq(void) {}
static int devm_request_irq(void *dev, int irq, irq_handler_t fn,
    int flags, const char *name, void *data) {
    (void)dev; (void)irq; (void)fn; (void)name; (void)data;
    assert(flags & IRQF_NO_AUTOEN); requested++;
    return requested==fail_request ? -16 : 0;
}
static void devapc_dump_info(bool boot) { assert(boot && !enabled); dumped=true; }
static void start_devapc(void) { assert(dumped && !enabled); started=true; }
static void enable_irq(int irq) { assert(started && (irq==335 || irq==336)); enabled++; }
static int probe(struct platform_device *pdev, int irq_type_num) {
    int ret, irq_type;
REQUEST_LOOP
FINISH
int main(void) {
    for (int n=1;n<=2;n++) for (int failure=0;failure<=n;failure++) {
        requested=enabled=0; dumped=started=false; fail_request=failure;
        assert(probe(&device,n)==(failure ? -16 : 0));
        assert(enabled==(failure ? 0 : n));
        assert(started==!failure && dumped==!failure);
    }
}
'''.replace("REQUEST_LOOP", request).replace("FINISH", finish)
with tempfile.TemporaryDirectory(prefix="nord2-devapc-") as directory:
    path = Path(directory)
    for name, code in (("violations", harness), ("ordering", ordering)):
        (path / (name + ".c")).write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                        str(path / (name + ".c")), "-o", str(path / name)], check=True)
        subprocess.run([str(path / name)], check=True)
print("PASS: startup status is acknowledged; runtime dump/ISR policy is retained; IRQ failures and enable ordering checked")
