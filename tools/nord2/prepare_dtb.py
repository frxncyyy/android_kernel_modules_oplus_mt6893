#!/usr/bin/env python3
"""Add verified Nord 2 CPU supplies to a plain MT6893 DTB.

The input is a device-owned DTB, not an Android DT table. fdtget and fdtput
from dtc must be available. Output is replaced only after all checks pass.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


RAILS = ('6_vbuck1', '7_vbuck3', 'vsram_proc1', 'vsram_proc2')


def prepare(source, output, power=False):
    def get(*args):
        return subprocess.check_output(
            ['fdtget', *map(str, args)], text=True).strip()

    compatible = get('-t', 's', source, '/', 'compatible').split()
    if 'mediatek,MT6893' not in compatible:
        raise ValueError('Expected an MT6893 DTB')
    if get('-t', 's', source, '/mt_cpufreq', 'compatible') != 'mediatek,mt-cpufreq':
        raise ValueError('Expected the legacy CPU-frequency node')

    def nodes(node='/'):
        yield node
        for child in get('-l', source, node).splitlines():
            yield from nodes(node.rstrip('/') + '/' + child)

    rails = {}
    for node in nodes():
        if 'regulator-name' not in get('-p', source, node).splitlines():
            continue
        name = get('-t', 's', source, node, 'regulator-name')
        if name not in RAILS:
            continue
        if name in rails:
            raise ValueError('Ambiguous regulator: ' + name)
        phandle = get('-t', 'x', source, node, 'phandle')
        if len(phandle.split()) != 1 or int(phandle, 16) in (0, 0xffffffff):
            raise ValueError('Invalid regulator phandle: ' + name)
        rails[name] = phandle
    if set(rails) != set(RAILS):
        raise ValueError('Missing CPU regulators: ' + ', '.join(set(RAILS) - set(rails)))
    if len(set(rails.values())) != len(RAILS):
        raise ValueError('CPU regulators must have distinct phandles')

    properties = get('-p', source, '/mt_cpufreq').splitlines()
    for name, phandle in rails.items():
        prop = name + '-supply'
        if prop in properties and get('-t', 'x', source, '/mt_cpufreq', prop) != phandle:
            raise ValueError('Existing supply conflicts with verified rail: ' + prop)

    output = Path(output)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        staged = Path(tmp) / 'prepared.dtb'
        shutil.copyfile(source, staged)
        staged.chmod(0o600)
        for name, phandle in rails.items():
            subprocess.run(['fdtput', '-t', 'x', str(staged), '/mt_cpufreq',
                            name + '-supply', phandle], check=True)
            assert get('-t', 'x', staged, '/mt_cpufreq', name + '-supply') == phandle
        if power:
            # The bootloader's vendor DTBO adds the actual gauge/charger nodes.
            # Keep their bindings; opt this board into its dedicated monitor.
            if 'oneplus,denniz' not in compatible:
                compatible.insert(0, 'oneplus,denniz')
            subprocess.run(['fdtput', '-t', 's', str(staged), '/', 'compatible',
                            *compatible], check=True)
        os.replace(staged, output)
    return rails


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--power', action='store_true',
                        help='identify this DTB as Nord 2 for its power monitor')
    args = parser.parse_args()
    prepare(args.input, args.output, args.power)
    print('Prepared Nord 2 DTB with four explicit CPU regulator supplies')
