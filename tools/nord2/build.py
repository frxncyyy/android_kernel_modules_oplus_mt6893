#!/usr/bin/env python3
"""Build the experimental Nord 2 kernel and device modules with one configuration."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]
MODULES = REPO / 'kernel/kernel_device_modules-6.6'
TOUCH = REPO / 'vendor/oplus/kernel/touchpanel/oplus_touchscreen_v2'


def config_values(path):
    result = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'(CONFIG_\w+)=(.*)', line)
        disabled = re.fullmatch(r'# (CONFIG_\w+) is not set', line)
        if match:
            result[match[1]] = match[2]
        elif disabled:
            result[disabled[1]] = 'n'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=REPO.parent / 'kernel-6.6')
    parser.add_argument('--out', type=Path, help='Defaults to out-nord2 beside the kernel checkout')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--configure-only', action='store_true')
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    out = (args.out or kernel.parent / 'out-nord2').resolve()
    if not (kernel / 'Makefile').is_file():
        parser.error(f'No kernel Makefile in {kernel}')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    out.mkdir(parents=True, exist_ok=True)
    # Vendor Kbuild files interpret these paths relative to both srctree and
    # objtree. Use a common ancestor instead of an absolute M= path.
    common = Path(os.path.commonpath([kernel, out, MODULES]))
    kernel_depth = len(kernel.relative_to(common).parts)
    out_depth = len(out.relative_to(common).parts)
    if kernel_depth != out_depth:
        parser.error('--out must have the same depth below the common ancestor as --kernel; a sibling directory works')
    relative = '../' * kernel_depth + str(MODULES.relative_to(common))
    for base in (kernel, out):
        if (base / relative).resolve() != MODULES:
            parser.error('The module path must resolve identically from source and output')
    make = ['make', '-C', str(kernel), f'O={out}', 'ARCH=arm64', 'LLVM=1',
            f'KCONFIG_EXT_PREFIX={relative}/', f'DEVICE_MODULES_REL_DIR={relative}',
            f'DEVICE_MODULES_PATH={MODULES}',
            f'DEVCIE_MODULES_INCLUDE=-I{MODULES}/include -I{MODULES}/include/uapi']
    for variable in ('HOSTCC', 'HOSTCFLAGS', 'HOSTLDFLAGS'):
        if variable in os.environ:
            make.append(f'{variable}={os.environ[variable]}')
    logs = out / 'logs'
    logs.mkdir(exist_ok=True)

    def run(label, command):
        log = logs / f'{label}.log'
        print(f'{label}: {log}', flush=True)
        with log.open('w') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            print('\n'.join(log.read_text(errors='replace').splitlines()[-40:]))
            raise SystemExit(result.returncode)

    run('gki-defconfig', make + ['gki_defconfig'])
    gki = out / 'gki.config'
    shutil.copyfile(out / '.config', gki)
    fragments = [MODULES / 'arch/arm64/configs/mgk_64_k66_defconfig',
                 MODULES / 'kernel/configs/mt6893_overlay.config',
                 MODULES / 'kernel/configs/nord2_bringup.config']
    run('merge-config', ['bash', str(kernel / 'scripts/kconfig/merge_config.sh'),
                        '-m', '-O', str(out), str(gki), *map(str, fragments)])
    # External-module make does not refresh auto.conf after olddefconfig.
    run('prepare', make + ['-j1', 'olddefconfig', 'prepare', 'modules_prepare'])
    actual = config_values(out / '.config')
    requested = config_values(fragments[-1])
    mismatches = {key: {'requested': value, 'actual': actual.get(key, 'n')}
                  for key, value in requested.items() if actual.get(key, 'n') != value}
    if mismatches:
        raise SystemExit('Nord 2 configuration was not honored:\n' + json.dumps(mismatches, indent=2))
    if args.configure_only:
        return
    run('kernel', make + [f'-j{args.jobs}', 'Image', 'modules'])
    run('vendor', make + [f'M={relative}', f'-j{args.jobs}', 'modules'])
    # Touch has a separate Kbuild root and Makefile-only configuration flags.
    # Reuse the notifier already built by the device-modules root; building its
    # copy here would produce a second module with the same name and exports.
    touch_relative = '../' * kernel_depth + str(TOUCH.relative_to(common))
    for base in (kernel, out):
        if (base / touch_relative).resolve() != TOUCH:
            parser.error('The touch path must resolve identically from source and output')
    touch_flags = ' '.join(filter(None, [os.environ.get('KCFLAGS'),
                          f'-I{MODULES}/include', f'-I{MODULES}/include/uapi']))
    run('touch', make + [f'M={touch_relative}', f'-j{args.jobs}', 'modules',
        f'KCFLAGS={touch_flags}', f'KBUILD_EXTRA_SYMBOLS={MODULES}/Module.symvers',
        'CONFIG_TOUCHPANEL_OPLUS=m', 'CONFIG_TOUCHPANEL_CUSTOM=m',
        'CONFIG_TOUCHPANEL_FOCAL=m', 'CONFIG_TOUCHPANEL_FOCAL_FT3518=m',
        'CONFIG_TOUCHPANEL_MTK_PLATFORM=y', 'CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY=y',
        'CONFIG_TOUCHPANEL_NOTIFY=n'])
    touch_modules = [TOUCH / path for path in (
        'oplus_bsp_tp_comon.ko', 'touch_custom/oplus_bsp_tp_custom.ko',
        'Focal/oplus_bsp_tp_focal_common.ko', 'Focal/ft3518/oplus_bsp_tp_ft3518.ko')]

    manifest = {
        'kernel_revision': subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', 'HEAD'], text=True).strip(),
        'modules_revision': subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip(),
        'kernel_dirty': bool(subprocess.check_output(['git', '-C', str(kernel), 'status', '--porcelain', '--untracked-files=normal'], text=True).strip()),
        'modules_dirty': bool(subprocess.check_output(['git', '-C', str(REPO), 'status', '--porcelain', '--untracked-files=normal'], text=True).strip()),
        'clang': subprocess.check_output(['clang', '--version'], text=True).splitlines()[0],
        'config_sha256': hashlib.sha256((out / '.config').read_bytes()).hexdigest(),
        'image_sha256': hashlib.sha256((out / 'arch/arm64/boot/Image').read_bytes()).hexdigest(),
        'touch_modules': {str(path.relative_to(REPO)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in touch_modules},
        'hardware_tested': False,
    }
    (out / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Kernel, device modules and FT3518 touch modules built. No boot image was packaged or flashed.')


if __name__ == '__main__':
    main()
