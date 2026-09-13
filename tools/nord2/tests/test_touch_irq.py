#!/usr/bin/env python3
"""Exercise IRQ registration plus the actual probe-completion assignment."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "vendor/oplus/kernel/touchpanel/oplus_touchscreen_v2/touchpanel_common_driver.c"
text = source.read_text()
start = text.index("int tp_register_irq_func(")
end = text.index("\nvoid tp_delta_read_triggered_by_key", start)
register = text[start:end]
start = text.index("\tif (ts->is_noflash_ic || ts->bus_type == TP_BUS_SPI) {", text.index("ts->report_rate_test_time = 5;"))
end = text.index("\n\n\tmutex_lock(&tp_core_lock);", start)
finish = text[start:end]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define TP_BUS_SPI 1
#define IRQF_ONESHOT 0x2000
#define TP_DEBUG(...) do {} while (0)
#define TP_BOOT_INFO(...) do {} while (0)
#define TP_INFO(...) do {} while (0)
struct client { int irq; } i2c, spi;
struct touchpanel_data {
    struct { int irq_gpio; } hw_res;
    int irq, irq_flags, irq_flags_cover, tp_index, bus_type;
    bool is_noflash_ic;
    struct client *client, *s_client;
    char irq_name[32]; void *dev;
} state;
static int mapped, requests, requested_irq, request_result;
static int gpio_is_valid(int gpio) { return gpio>=0; }
static int gpio_to_irq(int gpio) { (void)gpio; return mapped; }
static void tp_irq_thread_fn(void) {}
static int devm_request_threaded_irq(void *dev, int irq, void *top,
    void (*thread)(void), int flags, const char *name, void *data)
{
    (void)dev; (void)top; (void)thread; (void)flags; (void)name; (void)data;
    requests++; requested_irq=irq; return request_result;
}
REGISTER
static void finish_probe(struct touchpanel_data *ts)
{
FINISH
}
static void reset(int bus, bool noflash)
{
    i2c.irq=spi.irq=0;
    state=(struct touchpanel_data){ .hw_res={533}, .client=&i2c,
        .s_client=&spi, .bus_type=bus, .is_noflash_ic=noflash };
    mapped=36; requests=0; requested_irq=-1; request_result=0;
}
int main(void)
{
    for (int bus=0;bus<2;bus++) for (int noflash=0;noflash<2;noflash++) {
        for (int supplied=0;supplied<2;supplied++) {
            reset(bus,noflash);
            if (supplied) state.irq=41;
            assert(tp_register_irq_func(&state)==0);
            assert(requests==1 && requested_irq==(supplied ? 41 : 36));
            finish_probe(&state);
            assert(state.irq==requested_irq);
            struct client *client=(bus==TP_BUS_SPI || noflash) ? &spi : &i2c;
            assert(client->irq==requested_irq);
            /* The first resume must release the IRQ that probe requested. */
            int released_irq=state.irq;
            assert(released_irq==requested_irq);
            assert(tp_register_irq_func(&state)==0);
            assert(requested_irq==released_irq);
        }
        for (int error=0;error<2;error++) {
            reset(bus,noflash); mapped=error ? -517 : 0;
            assert(tp_register_irq_func(&state)==(error ? -517 : -EINVAL));
            assert(requests==0 && i2c.irq==0 && spi.irq==0);
        }
        reset(bus,noflash); request_result=-EBUSY;
        assert(tp_register_irq_func(&state)==-EBUSY);
        reset(bus,noflash); state.hw_res.irq_gpio=-1;
        assert(tp_register_irq_func(&state)<0 && requests==0);
    }
}
'''.replace("REGISTER", register).replace("FINISH", finish)
with tempfile.TemporaryDirectory(prefix="nord2-touch-irq-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: GPIO IRQ survives probe completion for I2C/SPI; invalid mappings are rejected")
