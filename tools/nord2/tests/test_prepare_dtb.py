#!/usr/bin/env python3
"""Exercise CPU regulator resolution against compiled synthetic device trees."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('prepare_dtb', Path(__file__).resolve().parents[1] / 'prepare_dtb.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()

with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    def tree(omit=None, duplicate=False, conflict=False):
        regulators = ''.join(f'r{i} {{ regulator-name = "{name}"; phandle = <{41+i*17}>; }};' for i, name in enumerate(module.RAILS) if name != omit)
        if duplicate:
            regulators += 'duplicate { regulator-name = "6_vbuck1"; phandle = <501>; };'
        supply = '6_vbuck1-supply = <501>;' if conflict else ''
        (d/'in.dts').write_text('/dts-v1/; / { compatible = "mediatek,MT6893", "test,retained"; mt_cpufreq { compatible = "mediatek,mt-cpufreq"; '+supply+' }; '+regulators+' };')
        subprocess.run(['dtc','-q','-I','dts','-O','dtb','-o',str(d/'in.dtb'),str(d/'in.dts')],check=True)
    tree()
    original = (d/'in.dtb').read_bytes()
    module.prepare(d/'in.dtb', d/'out.dtb', power=True)
    assert (d/'in.dtb').read_bytes() == original
    assert run('fdtget','-t','s',d/'out.dtb','/','compatible') == 'oneplus,denniz mediatek,MT6893 test,retained'
    for i, name in enumerate(module.RAILS):
        assert int(run('fdtget','-t','x',d/'out.dtb','/mt_cpufreq',name+'-supply'),16) == 41+i*17
    output = (d/'out.dtb').read_bytes()
    module.prepare(d/'out.dtb', d/'out.dtb', power=True)
    assert (d/'out.dtb').read_bytes() == output
    assert (d/'out.dtb').stat().st_mode & 0o777 == 0o600
    for args in ({'omit':'vsram_proc2'}, {'duplicate':True}, {'conflict':True}):
        tree(**args)
        try:
            module.prepare(d/'in.dtb', d/'out.dtb')
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid input accepted: '+str(args))
        assert (d/'out.dtb').read_bytes() == output
print('PASS: arbitrary phandles, CPU supplies, board opt-in, idempotency, protected output, invalid input preserves output')
