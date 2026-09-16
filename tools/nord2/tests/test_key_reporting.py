#!/usr/bin/env python3
"""Check the button and alert-slider contracts that hardware rounds confirmed."""
from pathlib import Path
import re

repo = Path(__file__).resolve().parents[3]
kpd = repo / 'kernel/kernel_device_modules-6.6/drivers/input/keyboard/mtk-kpd.c'
pmic = repo / 'kernel/kernel_device_modules-6.6/drivers/input/keyboard/mtk-pmic-keys.c'
tri = repo / 'vendor/oplus/kernel/device_info/tri_state_key'
build = repo / 'tools/nord2/build.py'

kpd_text = kpd.read_text()
# The SoC keypad matrix carries no key on this board and the DTB still maps its
# slot 0 to KEY_VOLUMEDOWN, so scanning it would double-report volume down.
assert 'enable_kpd(keypad->base, 0);' in kpd_text, 'mtk-kpd must leave the keypad matrix off'
assert 'enable_kpd(keypad->base, 1)' not in kpd_text, 'nothing may switch the keypad matrix on'
assert 'KP_EN' in kpd_text and 'kpd_keymap_handler' in kpd_text, 'unexpected mtk-kpd layout'
# Volume up has no EINT on this board and keypad,volume-up points at the touch
# panel reset line, so the GPIO may only be claimed when the EINT node exists.
assert 'VOLUME_UP-eint' in kpd_text, 'mtk-kpd must probe for the volume-up EINT node'
assert re.search(r'vol_key_info\.oplus_vol_up_flag\s*=\s*kpd_volkey_has_eint\(', kpd_text), \
    'the volume-up GPIO must stay gated on the EINT node'
assert 'VOLUME_DOWN-eint' in kpd_text, 'volume down must come from the EINT node'

pmic_text = pmic.read_text()
assert '"mediatek,mt-pmic"' in pmic_text, 'mtk-pmic-keys must bind the legacy node the DTB has'
assert '"pwrkey"' in pmic_text and '"homekey"' in pmic_text, 'the PMIC IRQs are requested by name'
# The board reports volume up through the PMIC home key because the DTB sets
# mediatek,kpd-sw-rstkey; the keycode must be read from there, not assumed.
assert 'kpd-sw-rstkey' in pmic_text, 'the home key keycode must follow mediatek,kpd-sw-rstkey'
assert 'KEY_VOLUMEUP' in pmic_text and 'KEY_POWER' in pmic_text, 'expected power and volume-up keys'

tri_text = (tri / 'oplus_tri_key.c').read_text()
api = (tri / 'tri_key_common_api.h').read_text()
assert '"oplus,hall_tri_state_key"' in api, 'the tri-state input device keeps its vendor name'
for value in (1, 2, 3):
    assert re.search(r'input_report_key\(chip->input_dev, KEY_F3, %d\)' % value, tri_text), \
        'position %d must be reported as KEY_F3' % value
assert 'input_report_key(chip->input_dev, KEY_F3, 0)' in tri_text, 'each position must be released'
assert 'module_init' not in tri_text, 'oplus_tri_key.c is a symbol provider, not a driver'
assert 'oplus_register_hall' in tri_text, 'the hall drivers need the registration entry point'
for name in ('hall_mxm1120_up.c', 'hall_mxm1120_down.c'):
    hall = (tri / 'hall_ic' / name).read_text()
    assert 'oplus_register_hall' in hall, name + ' must register with the tri-state core'
assert '"oplus,hall-mxm1120,up"' in (tri / 'hall_ic/hall_mxm1120_up.c').read_text()
assert '"oplus,hall-mxm1120,down"' in (tri / 'hall_ic/hall_mxm1120_down.c').read_text()

# The three modules need their own Kbuild root, and the notifier branch has to be
# defined for the preprocessor or the fbdev fallback fails the tree's -Werror.
build_text = build.read_text()
assert "TRIKEY = REPO / 'vendor/oplus/kernel/device_info/tri_state_key'" in build_text
for flag in ("'CONFIG_OPLUS_TRIKEY_MAIN=m'", "'CONFIG_OPLUS_TRIKEY_HALL=m'",
             "'CONFIG_MXM_UP=m'", "'CONFIG_MXM_DOWN=m'"):
    assert flag in build_text, flag + ' missing from the tri-state build'
assert '-DCONFIG_OPLUS_MTK_DRM_GKI_NOTIFY=1' in build_text, 'the display notifier must be compiled in'
assert "'oplus_bsp_tri_key.ko', 'hall_ic/oplus_bsp_mxm_up.ko'" in build_text, \
    'the manifest must record the hall ICs beside the core'

print('PASS: matrix stays off, volume-up GPIO stays gated, PMIC keys follow the DTB, '
      'slider reports KEY_F3 1/2/3 and all three modules are built')
