#!/usr/bin/env python3
"""Wrap the board's own device tree into the MediaTek boot table LK consumes.

The table that follows the boot image is a 64-byte header (magic 0xd7b7ab1e,
total size, then the embedded DTB size at offset 32 and its offset at 36)
followed by exactly one device tree.  LK reads that tree, and the kernel
inherits whatever it says.

The stock table carries a plain ``mediatek,MT6893`` tree.  Flashing it
unchanged drops the two board-specific facts this port depends on:

* the root ``compatible`` that opts the machine into the OnePlus power
  monitor, which is the only thing that lets ``nord2-power`` bind its
  ``oplus,bq27541-battery`` gauge and ``oplus,mp2650-charger`` port, and
* the four CPU regulator supplies that let ``mt_cpufreq`` resolve its rails.

``prepare_dtb.prepare(..., power=True)`` adds both.  This script applies it,
forces the diagnostic USB role, and rewraps the result, asserting the board
facts survived so a packaged image can never silently ship the plain tree.

Usage:
    build_boot_dtb.py device.dtb backing-table.bin base.dtb table.bin \
        [--power] [--usb-role peripheral]
"""

import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

MAGIC = 0xD7B7AB1E
HEADER = 64
SIZE_FIELD = 4
DTB_SIZE_FIELD = 32
DTB_OFFSET_FIELD = 36

_spec = importlib.util.spec_from_file_location(
    'prepare_dtb', Path(__file__).resolve().parent / 'prepare_dtb.py')
prepare_dtb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(prepare_dtb)


def fdtget(*args):
    return subprocess.check_output(['fdtget', *map(str, args)], text=True).strip()


def split_table(blob):
    """Return (header, dtb) from a MediaTek boot table."""
    if len(blob) < HEADER:
        raise ValueError('Boot table is shorter than its header')
    magic, total = struct.unpack_from('>II', blob, 0)
    dtb_size, dtb_offset = struct.unpack_from('>II', blob, DTB_SIZE_FIELD)
    if magic != MAGIC:
        raise ValueError('Unexpected boot table magic %#x' % magic)
    if dtb_offset != HEADER:
        raise ValueError('Unexpected DTB offset %d' % dtb_offset)
    if total != len(blob):
        raise ValueError('Boot table size field %d does not match %d' % (total, len(blob)))
    if dtb_size != len(blob) - HEADER:
        raise ValueError('Embedded DTB size field %d does not match %d'
                         % (dtb_size, len(blob) - HEADER))
    return blob[:HEADER], blob[HEADER:]


def join_table(header, dtb):
    """Return a boot table carrying ``dtb`` behind the original header."""
    if len(header) != HEADER:
        raise ValueError('Header must be exactly %d bytes' % HEADER)
    table = bytearray(header)
    struct.pack_into('>I', table, SIZE_FIELD, HEADER + len(dtb))
    struct.pack_into('>I', table, DTB_SIZE_FIELD, len(dtb))
    return bytes(table) + dtb


def check(source, power, usb_role):
    """Assert the board-specific facts the port needs are present."""
    compatible = fdtget('-t', 's', source, '/', 'compatible').split()
    if 'mediatek,MT6893' not in compatible:
        raise ValueError('Expected an MT6893 device tree')
    if power and 'oneplus,denniz' not in compatible:
        raise ValueError('Board compatible oneplus,denniz missing')
    if power:
        properties = fdtget('-p', source, '/mt_cpufreq').splitlines()
        for name in prepare_dtb.RAILS:
            if name + '-supply' not in properties:
                raise ValueError('CPU supply %s-supply missing' % name)
    if usb_role:
        try:
            role = fdtget('-t', 's', source, '/usb0@11201000', 'dr_mode')
        except subprocess.CalledProcessError:
            role = None
        if role is not None and role != usb_role:
            raise ValueError('usb0 dr_mode is %r, expected %r' % (role, usb_role))
    return compatible



# The device DTB is the stock 4.19 tree, so its smi_larb* nodes name the second clock with
# 4.19-era names ("venc-set1", "vdec-larb", "cam-larb13", ...).  The 6.6 clock drivers in
# this tree register the same gates under new names (ven1_cke0_larb, vde1_larb1_cken,
# cam_m_larb13, ...).  mtk-smi.c treats a failed devm_clk_get as fatal:
#
#     larb->smi.clks[i] = devm_clk_get(dev, name);
#     if (IS_ERR(...)) { dev_info(dev, "CLK%d:%s get failed\n", i, name); return PTR_ERR(...); }
#
# so every affected LARB fails to probe, SMI never comes up, dispsys_config never completes
# and the boot does not finish.  Rename the property in place; the clock *index* is
# unchanged, so only the name string needs to move.
LARB_CLOCK_ALIASES = {
    'venc-set1': 'ven1_cke0_larb',
    'venc-c1-set1': 'ven2_cke0_larb',
    'vdec-larb': 'vde1_larb1_cken',
    'vdec-soc-larb': 'vde2_larb1_cken',
    'ipe-subcom': 'ipe_smi_subcom',
    'img1-larb9': 'imgsys1_larb9',
    'img2-larb11': 'imgsys2_larb9',
    'cam-larb13': 'cam_m_larb13',
    'cam-larb14': 'cam_m_larb14',
    'cam-larb15': 'cam_m_larb15',
    'cam-rawa-larb': 'cam_ra_larbx',
    'cam-rawb-larb': 'cam_rb_larbx',
    'cam-rawc-larb': 'cam_rc_larbx',
}


def fix_larb_clocks(staged):
    """Rewrite 4.19-era smi_larb clock-names to the names the 6.6 drivers register."""
    changed = []
    for node in fdtget('-l', str(staged), '/').splitlines():
        if not (node.startswith('smi_larb') or node.startswith('ipe_smi_subcom')):
            continue
        path = '/' + node
        try:
            names = fdtget('-t', 's', str(staged), path, 'clock-names').split()
        except subprocess.CalledProcessError:
            continue
        new = [LARB_CLOCK_ALIASES.get(n, n) for n in names]
        if new != names:
            subprocess.run(['fdtput', '-t', 's', str(staged), path,
                            'clock-names', *new], check=True)
            changed.append((node, names, new))
    return changed


def build(device_dtb, backing_table, base_output, table_output,
          power=True, usb_role='peripheral'):
    """Write the prepared device tree and its boot table."""
    device_dtb = Path(device_dtb)
    backing_table = Path(backing_table)
    base_output = Path(base_output)
    table_output = Path(table_output)
    if device_dtb.read_bytes() == backing_table.read_bytes():
        raise ValueError('Device DTB and backing table must come from different sources')

    header, embedded = split_table(backing_table.read_bytes())
    if embedded[:4] != b'\xd0\x0d\xfe\xed':
        raise ValueError('Backing table does not carry an FDT')

    base_output.parent.mkdir(parents=True, exist_ok=True)
    table_output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base_output.parent) as tmp:
        staged = Path(tmp) / 'base.dtb'
        prepare_dtb.prepare(device_dtb, staged, power=power)
        if usb_role:
            subprocess.run(['fdtput', '-t', 's', str(staged),
                            '/usb0@11201000', 'dr_mode', usb_role], check=True)
        fix_larb_clocks(staged)
        check(staged, power, usb_role)
        base = staged.read_bytes()
        table = join_table(header, base)
        _, round_trip = split_table(table)
        if round_trip != base:
            raise ValueError('Rewrapped table does not carry the prepared DTB')
        shutil.copyfile(staged, base_output)
    base_output.chmod(0o600)
    table_output.write_bytes(table)
    table_output.chmod(0o600)
    return base, table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('device_dtb', type=Path,
                        help='device-owned DTB, e.g. the recovery partition tree')
    parser.add_argument('backing_table', type=Path,
                        help='stock LK boot table to take the header from')
    parser.add_argument('base_output', type=Path)
    parser.add_argument('table_output', type=Path)
    parser.add_argument('--power', action='store_true',
                        help='identify this DTB as Nord 2 and keep its CPU rails')
    parser.add_argument('--usb-role', default='peripheral')
    args = parser.parse_args()
    base, table = build(args.device_dtb, args.backing_table, args.base_output,
                        args.table_output, power=args.power, usb_role=args.usb_role)
    print('DTB %d bytes; boot table %d bytes' % (len(base), len(table)))


if __name__ == '__main__':
    main()
