#!/usr/bin/env python3
"""Exercise boot-table wrapping and the board fact check on synthetic trees."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_boot_dtb', HERE / 'build_boot_dtb.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)

    def device_tree(rails=True, usb='peripheral', out='device.dtb'):
        regulators = ''.join(f'r{i} {{ regulator-name = "{name}"; phandle = <{41+i*17}>; }};'
                             for i, name in enumerate(module.prepare_dtb.RAILS)) if rails else ''
        usb_node = 'usb0@11201000 { dr_mode = "%s"; };' % usb if usb else ''
        (d / 'device.dts').write_text(
            '/dts-v1/; / { compatible = "mediatek,MT6893"; '
            'mt_cpufreq { compatible = "mediatek,mt-cpufreq"; }; %s %s };' % (regulators, usb_node))
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb',
                        '-o', str(d / out), str(d / 'device.dts')], check=True)

    def backing_table():
        # The stock table carries the plain tree, as the unpacked boot image does.
        device_tree(out='plain.dtb')
        header = bytearray(64)
        struct.pack_into('>I', header, 0, module.MAGIC)
        plain = (d / 'plain.dtb').read_bytes()
        struct.pack_into('>I', header, module.SIZE_FIELD, 64 + len(plain))
        struct.pack_into('>I', header, module.DTB_SIZE_FIELD, len(plain))
        struct.pack_into('>I', header, module.DTB_OFFSET_FIELD, 64)
        (d / 'table.bin').write_bytes(bytes(header) + plain)

    device_tree()
    backing_table()

    # --power adds the board compatible and keeps the CPU rails.
    base, table = module.build(d / 'device.dtb', d / 'table.bin',
                               d / 'base.dtb', d / 'out.bin', power=True, usb_role='peripheral')
    _, embedded = module.split_table(table)
    assert embedded == base == (d / 'base.dtb').read_bytes()
    assert struct.unpack_from('>I', table, module.SIZE_FIELD)[0] == len(table)
    assert struct.unpack_from('>I', table, module.DTB_SIZE_FIELD)[0] == len(base)
    assert run('fdtget', '-t', 's', d / 'base.dtb', '/', 'compatible') == 'oneplus,denniz mediatek,MT6893'
    assert run('fdtget', '-t', 's', d / 'base.dtb', '/usb0@11201000', 'dr_mode') == 'peripheral'
    for i, name in enumerate(module.prepare_dtb.RAILS):
        assert int(run('fdtget', '-t', 'x', d / 'base.dtb', '/mt_cpufreq', name + '-supply'), 16) == 41 + i * 17

    # Without --power the board stays anonymous, as the stock table leaves it.
    module.build(d / 'device.dtb', d / 'table.bin', d / 'base.dtb', d / 'out.bin',
                 power=False, usb_role=None)
    assert run('fdtget', '-t', 's', d / 'base.dtb', '/', 'compatible') == 'mediatek,MT6893'

    # A device tree without the CPU regulators cannot be prepared.
    device_tree(rails=False, out='norails.dtb')
    try:
        module.build(d / 'norails.dtb', d / 'table.bin', d / 'base.dtb', d / 'out.bin', power=True)
    except ValueError:
        pass
    else:
        raise AssertionError('Device tree without CPU regulators accepted')

    # A backing table whose size fields disagree is rejected.
    blob = bytearray((d / 'table.bin').read_bytes())
    struct.pack_into('>I', blob, module.DTB_SIZE_FIELD, len(blob))
    (d / 'bad.bin').write_bytes(bytes(blob))
    try:
        module.build(d / 'device.dtb', d / 'bad.bin', d / 'base.dtb', d / 'out.bin')
    except ValueError:
        pass
    else:
        raise AssertionError('Boot table with a bad size field accepted')
print('PASS: boot table round trip, board facts, --power requirement, header validation')
