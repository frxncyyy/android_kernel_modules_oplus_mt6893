#!/usr/bin/env python3
"""Exercise the port's API adapters and power failures with mocked hardware.

Compiles the production functions, not copies of their implementations.
This checks software behavior only; it does not validate electrical timings.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'vendor/oplus/kernel_device_modules-6.6/drivers/gpu/drm/oplus/oplus_panel/oplus20615_samsung_ams643ye05_1080p_dsi_cmd.c'


def definition(source, name):
    match = re.search(r'^static [^\n]+\b' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise ValueError(name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


class PanelPortTest(unittest.TestCase):
    def test_adapters_and_power_failures(self):
        source = SOURCE.read_text()
        tables = []
        for name in ('lcm_seed_mode0', 'lcm_seed_mode1'):
            tables.append(re.search(r'static struct LCM_setting_table ' + name + r'\[\] = \{.*?\n\};', source, re.S).group())
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define REGFLAG_CMD 0xfffa
#define REGFLAG_END_OF_TABLE 0xfffd
struct LCM_setting_table { unsigned cmd; unsigned char count; unsigned char para_list[129]; };
struct drm_display_mode { int fps; struct drm_display_mode *next; };
struct mode_list { struct drm_display_mode *first; };
struct drm_connector { struct mode_list modes; };
#define list_for_each_entry(mode, head, member) \
    for ((mode) = (head)->first; (mode); (mode) = (mode)->next)
static int drm_mode_vrefresh(struct drm_display_mode *m) { return m->fps; }
struct lcm;
struct drm_panel { struct lcm *ctx; };
struct mtk_panel_params { int fps; };
struct mtk_panel_ext { struct mtk_panel_params *params; };
static struct mtk_panel_params ext_params = {60}, ext_params_90hz = {90};
static struct mtk_panel_ext ext = {&ext_params};
static struct mtk_panel_ext *available_ext = &ext;
static struct mtk_panel_ext *find_panel_ext(struct drm_panel *p) { (void)p; return available_ext; }
struct mtk_dsi; struct cmdq_pkt;
struct mtk_ddic_cmd { unsigned cmd_num; unsigned char *para_list; };
struct mtk_ddic_dsi_cmd { unsigned is_package, is_hs, cmd_count; struct mtk_ddic_cmd mtk_ddic_cmd_table[32]; };
typedef void (*dcs_write_gce_pack)(struct mtk_dsi *, struct cmdq_pkt *, struct mtk_ddic_dsi_cmd *);
static struct mtk_ddic_dsi_cmd captured;
static unsigned packets;
static void capture(struct mtk_dsi *d, struct cmdq_pkt *h, struct mtk_ddic_dsi_cmd *p)
{ (void)d; (void)h; captured = *p; packets++; }
enum MTK_PANEL_MODE_SWITCH_STAGE { BEFORE_DSI_POWERDOWN, AFTER_DSI_POWERON };
struct lcm {
    struct drm_panel panel;
    void *vmc, *bias_gpio, *reset_gpio;
    unsigned oled_ldo;
    bool powered, vmc_enabled, oled_enabled, prepared;
    int error;
};
static struct lcm *panel_to_lcm(struct drm_panel *p) { return p->ctx; }
static int switched_fps;
static void mode_switch_60_to_90(struct drm_panel *p, enum MTK_PANEL_MODE_SWITCH_STAGE s)
{ (void)p; (void)s; switched_fps = 90; }
static void mode_switch_90_to_60(struct drm_panel *p, enum MTK_PANEL_MODE_SWITCH_STAGE s)
{ (void)p; (void)s; switched_fps = 60; }
static int set_error, enable_error, disable_error, oled_error, oled_off_error;
static int refs, enables, disables, oled_on, oled_off;
static int regulator_set_voltage(void *r, int lo, int hi)
{ (void)r; assert(lo == 3000000 && hi == lo); return set_error; }
static int regulator_enable(void *r)
{ (void)r; enables++; if (!enable_error) refs++; return enable_error; }
static int regulator_disable(void *r)
{ (void)r; disables++; if (!disable_error) refs--; assert(refs >= 0); return disable_error; }
static int pmic_ldo_set_voltage_mv(unsigned ldo, int mv)
{ assert(ldo == 5 && mv == 1504); oled_on++; return oled_error; }
static int pmic_ldo_set_disable(unsigned ldo)
{ assert(ldo == 5); oled_off++; return oled_off_error; }
static void gpiod_set_value_cansleep(void *p, int v) { (void)p; assert(v == 0 || v == 1); }
static void msleep(unsigned ms) { (void)ms; }
static int lcm_panel_poweroff(struct drm_panel *panel);
'''
        main = r'''
int main(void) {
    struct lcm ctx = {.oled_ldo = 5}; ctx.panel.ctx = &ctx;
    struct drm_display_mode m60 = {60, NULL}, m90 = {90, &m60};
    struct drm_connector conn = {{&m90}};
    /* Sorting the connector list must not swap the physical modes. */
    assert(mtk_panel_ext_param_set(&ctx.panel, &conn, 0) == 0 && ext.params->fps == 90);
    assert(mtk_panel_ext_param_set(&ctx.panel, &conn, 1) == 0 && ext.params->fps == 60);
    assert(mtk_panel_ext_param_set(&ctx.panel, &conn, 2) == -EINVAL && ext.params->fps == 60);
    assert(mtk_panel_ext_param_set(&ctx.panel, NULL, 0) == -EINVAL);
    available_ext = NULL;
    assert(mtk_panel_ext_param_set(&ctx.panel, &conn, 0) == -EINVAL);
    available_ext = &ext;
    assert(mode_switch(&ctx.panel, &conn, 1, 0, AFTER_DSI_POWERON) == 0 && switched_fps == 90);
    assert(mode_switch(&ctx.panel, &conn, 0, 1, AFTER_DSI_POWERON) == 0 && switched_fps == 60);
    switched_fps = 0;
    assert(mode_switch(&ctx.panel, &conn, 0, 0, AFTER_DSI_POWERON) == 0 && switched_fps == 0);
    assert(mode_switch(&ctx.panel, &conn, 0, 99, AFTER_DSI_POWERON) == -EINVAL);
    ctx.error = -EIO;
    assert(mode_switch(&ctx.panel, &conn, 0, 1, AFTER_DSI_POWERON) == -EIO);
    ctx.error = 0;
    /* The new callback takes one packet containing six complete commands. */
    assert(panel_set_seed_pack(NULL, capture, NULL, 101) == 0);
    assert(packets == 1 && captured.cmd_count == 6 && captured.is_package && captured.is_hs);
    assert(captured.mtk_ddic_cmd_table[4].cmd_num == 22);
    assert(captured.mtk_ddic_cmd_table[1].para_list[1] == 0x06);
    assert(panel_set_seed_pack(NULL, capture, NULL, 102) == 0);
    assert(packets == 2 && captured.mtk_ddic_cmd_table[1].para_list[1] == 0x86);
    assert(panel_set_seed_pack(NULL, capture, NULL, 99) == -EINVAL && packets == 2);
    assert(panel_set_seed_pack(NULL, NULL, NULL, 101) == -EINVAL);
    /* Every failure must leave a retryable state without leaking supply refs. */
    set_error = -EIO;
    assert(lcm_panel_poweron(&ctx.panel) == -EIO && enables == 0 && !ctx.powered);
    set_error = 0; enable_error = -EAGAIN;
    assert(lcm_panel_poweron(&ctx.panel) == -EAGAIN && refs == 0 && !ctx.vmc_enabled);
    enable_error = 0; oled_error = -EIO;
    assert(lcm_panel_poweron(&ctx.panel) == -EIO && refs == 0 && !ctx.powered);
    oled_error = 0;
    assert(lcm_panel_poweron(&ctx.panel) == 0 && refs == 1 && ctx.powered);
    int e = enables, o = oled_on;
    assert(lcm_panel_poweron(&ctx.panel) == 0 && enables == e && oled_on == o);
    ctx.prepared = true;
    assert(lcm_panel_poweroff(&ctx.panel) == 0 && refs == 1);
    ctx.prepared = false; disable_error = -EIO; oled_off_error = -EAGAIN;
    assert(lcm_panel_poweroff(&ctx.panel) == -EAGAIN && refs == 1);
    assert(ctx.vmc_enabled && ctx.oled_enabled && !ctx.powered);
    disable_error = oled_off_error = 0;
    assert(lcm_panel_poweroff(&ctx.panel) == 0 && refs == 0);
    assert(!ctx.vmc_enabled && !ctx.oled_enabled);
    int d = disables;
    assert(lcm_panel_poweroff(&ctx.panel) == 0 && disables == d);
    puts("YE05 mode, packed callback, and power-failure checks passed");
}
'''
        functions = '\n'.join(definition(source, name) for name in (
            'panel_mode_fps', 'mtk_panel_ext_param_set', 'mode_switch',
            'panel_set_seed_pack', 'lcm_panel_poweron', 'lcm_panel_poweroff'))
        with tempfile.TemporaryDirectory(prefix='ye05-test-') as temp:
            unit = Path(temp) / 'test.c'
            binary = Path(temp) / 'test'
            unit.write_text(prelude + '\n'.join(tables) + functions + main)
            subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
