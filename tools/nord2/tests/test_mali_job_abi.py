#!/usr/bin/env python3
"""Check the installed MT6893 driver's job layout with telemetry on and off."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
headers = root / 'vendor/mediatek/kernel_modules/gpu/gpu_mali/mali_avalon/mali-r49p1/drivers/gpu/arm/midgard/include/uapi/gpu/arm/midgard/jm'
source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else headers / 'mali_base_jm_kernel.h'
# The legacy UAPI used u32 with telemetry enabled. Keep the alias here so a
# negative control fails on layout, rather than an unrelated missing typedef.
fixture = r'''
#include <linux/types.h>
typedef __u32 u32;
#include "HEADER"
_Static_assert(sizeof(struct base_jd_atom_v2) == 64, "MT6893 v2 stride");
_Static_assert(sizeof(struct base_jd_atom) == 72, "MT6893 v3 stride");
_Static_assert(__builtin_offsetof(struct base_jd_atom_v2, frame_nr) == 56, "v2 frame offset");
_Static_assert(__builtin_offsetof(struct base_jd_atom, frame_nr) == 64, "v3 frame offset");
_Static_assert(__builtin_offsetof(struct base_jd_atom, jc) == 8, "v3 job-chain offset");
_Static_assert(__builtin_offsetof(struct base_jd_atom, core_req) == 52, "v3 requirements offset");
'''.replace('HEADER', str(source))
with tempfile.TemporaryDirectory() as tmp:
    test = Path(tmp) / 'layout.c'
    for board in ('CONFIG_MTK_GPU_MT6893_SUPPORT', 'CONFIG_MTK_GPU_MT6893_SUPPORT_MODULE', None):
        for enabled in (False, True):
            body = fixture
            if board is None and not enabled:
                body = '\n'.join(l for l in body.splitlines() if 'frame_nr' not in l)
                body = body.replace('== 64, "MT6893 v2 stride"', '== 56, "generic v2 stride"')
                body = body.replace('== 72, "MT6893 v3 stride"', '== 64, "generic v3 stride"')
            test.write_text(body)
            args = [os.environ.get('CC', 'cc'), '-std=c11', '-Werror', '-fsyntax-only', '-I'+str(headers)]
            if board:
                args += ['-D'+board+'=1']
            if enabled:
                args += ['-DCONFIG_MALI_MTK_GPU_BM_JM=1']
            subprocess.run(args+[str(test)], check=True)
print('MT6893 built-in/module ABI passes with telemetry on/off; generic layouts preserved')
