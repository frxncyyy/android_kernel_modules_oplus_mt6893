"""Check every module that will be shipped against the kernel's symbol CRCs.

The board profile changes CONFIG symbols between rounds, and the staged module
sets (display, GPU, CPU) are prebuilt from earlier rounds.  A config change
that altered the signature of a symbol they import would leave them unable to
load ("disagrees about version of symbol"), so compare the __versions section
of each module with the current Module.symvers rather than finding out on the
device.
"""
from pathlib import Path
import argparse, subprocess, sys
ap=argparse.ArgumentParser(description='Check the shipped modules against the kernel symbol CRCs')
ap.add_argument('--work',default='work',help='port workspace directory holding private/ and build/')
w=Path(ap.parse_args().work).resolve();out=w/'build/baseline'
modprobe=str(w/'tools/sysroot/usr/sbin/modprobe')

crc={}
def eat(path):
    for line in path.read_text(errors='replace').splitlines():
        parts=line.split('\t')
        if len(parts)>=2 and parts[1] and parts[1] not in crc:crc[parts[1]]=parts[0].lower()
eat(out/'Module.symvers')
for p in (w/'src/modules/vendor/mediatek/kernel_modules/connectivity').rglob('Module.symvers'):
    eat(p)

def boot_modules():
    d=next((w/'private/display-v11/staging/lib/modules').glob('*'))
    files=list(d.glob('*.ko'))
    files+=list((w/'private/gpu-v7').glob('*.ko'))
    files+=list((w/'private/cpu-v1').glob('*.ko'))
    files+=[w/'src/modules/vendor/mediatek/kernel_modules/connectivity'/p for p in
        ['conninfra/conninfra.ko','connfem/connfem.ko','wlan/adaptor/wmt_chrdev_wifi.ko',
         'wlan/core/gen4m/wlan_drv_gen4m_6893.ko','bt/mt66xx/6893/bt_drv_6893.ko']]
    files+=[w/'src/modules/kernel/kernel_device_modules-6.6'/p for p in
        ['drivers/misc/mediatek/connectivity/connadp.ko',
         'drivers/misc/mediatek/btif/common/btif_drv.ko',
         'drivers/input/keyboard/mtk-kpd.ko','drivers/input/keyboard/mtk-pmic-keys.ko',
         'drivers/leds/leds-mtk-disp.ko','drivers/i2c/busses/i2c-mt65xx.ko',
         'drivers/power/supply/nord2-power.ko',
         'drivers/gpu/drm/mediatek/mediatek_v2/mediatek-drm.ko']]
    files+=[out/'net/wireless/cfg80211.ko']
    return sorted({p for p in files if p.exists()})

mismatch=[];unknown={}
for m in boot_modules():
    try:
        dump=subprocess.check_output([modprobe,'--dump-modversions',str(m)],text=True,stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        dump=''
    miss=[]
    for line in dump.splitlines():
        parts=line.split('\t')
        if len(parts)!=2:continue
        want,sym=parts[0].lower(),parts[1]
        if sym not in crc:unknown.setdefault(m.name,[]).append(sym);continue
        if crc[sym]!=want:miss.append((sym,want,crc[sym]))
    if miss:mismatch.append((m.name,miss))
print('modules checked:',len(boot_modules()))
print('modules with CRC mismatches:',len(mismatch))
for name,miss in mismatch:
    for sym,want,have in miss[:8]:print('  %s: %s module=0x%s kernel=0x%s'%(name,sym,want[2:],have[2:]))
print('symbols the symvers does not describe:',sum(len(v) for v in unknown.values()))
sys.exit(1 if mismatch else 0)
