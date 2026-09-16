"""Re-stage the prebuilt module sets after the kernel source changes.

The port ships module sets that earlier rounds staged and verified: display,
touch, USB and storage in private/display-v11, the GPU set in private/gpu-v7 and
the CPU set in private/cpu-v1.  A module's vermagic embeds the kernel release
string, and that string carries the kernel commit hash, so any kernel change --
even a commit that only touches cfg80211 -- leaves every cached module unable to
load ("version magic ... should be ...").

Run the normal kernel build first: it relinks the modules at the paths the
manifests recorded.  This script copies those fresh files into a staging
directory named for the new release, re-runs depmod and the GKI symbol checks,
and only then removes the stale directory.
"""
from pathlib import Path
import argparse, collections, hashlib, json, re, shutil, subprocess, sys
ap=argparse.ArgumentParser(description='Re-stage the cached module sets for the current kernel release')
ap.add_argument('--work',default='work',help='port workspace directory holding private/ and build/')
w=Path(ap.parse_args().work).resolve();o=w/'build/baseline'
release=(o/'include/config/kernel.release').read_text().strip()
modinfo=str(w/'tools/sysroot/usr/sbin/modinfo');depmod=str(w/'tools/sysroot/usr/sbin/depmod')

def recorded(bundle):
    d=json.loads((w/'private'/bundle/'manifest.json').read_text())
    out={}
    for m in d.get('modules',[]):
        p=Path(m['path']);out[p.name]=p
    if not out:raise SystemExit(bundle+': manifest lists no modules')
    return out

def fresh(name,path):
    assert path.exists(),'%s: recorded path %s is missing; run the kernel build first'%(name,path)
    v=subprocess.check_output([modinfo,'-F','vermagic',str(path)],text=True).strip()
    assert v.split()[0]==release,'%s: %s has vermagic %s, want %s'%(name,path,v.split()[0],release)
    return path

def install(src,dst):
    shutil.copyfile(src,dst)
    subprocess.run(['llvm-strip','--strip-debug',str(dst)],check=True)

# ---- the display/touch/USB/storage set lives in a release-named directory ----
bundle='display-v11';b=w/'private'/bundle;modules=recorded(bundle)
staging=b/'staging';d=staging/'lib/modules'/release
old=[p for p in (staging/'lib/modules').glob('*') if p.name!=release]
assert not d.exists() or not any(d.glob('*.ko')),'%s already staged; remove it first'%d
d.mkdir(parents=True,exist_ok=True)
for name,path in sorted(modules.items()):install(fresh(name,path),d/name)
for f in ('modules.builtin','modules.builtin.modinfo'):shutil.copyfile(o/f,d/f)
options=None
for p in old+[d]:
    if (p/'modules.options').exists():options=p/'modules.options'
assert options,'no modules.options to carry forward'
shutil.copyfile(options,d/'modules.options')
(d/'modules.order').write_text(''.join(n+'\n' for n in sorted(modules)))
subprocess.run([depmod,'-b',str(staging),'-e','-F',str(o/'System.map'),release],check=True)
allow=set(re.findall(r'"([A-Za-z0-9_]+)"',(o/'include/generated/gki_module_unprotected.h').read_text()))
protected=set(re.findall(r'"([A-Za-z0-9_]+)"',(o/'include/generated/gki_module_protected_exports.h').read_text()))
exports=collections.defaultdict(list);imports={}
for p in d.glob('*.ko'):
    syms=subprocess.check_output(['llvm-nm',str(p)],text=True).splitlines()
    imports[p.name]={l.split()[-1] for l in syms if len(l.split())==2 and l.split()[0]=='U'}
    for l in syms:
        s=l.split()[-1]
        if s.startswith('__ksymtab_'):exports[s[len('__ksymtab_'):]].append(p.name)
bad={n:sorted(s-allow-set(exports)-{'__this_module'}) for n,s in imports.items() if s-allow-set(exports)-{'__this_module'}}
dup={s:ns for s,ns in exports.items() if len(ns)>1}
assert not bad,bad
assert not dup,dup
assert not set(exports)&protected,sorted(set(exports)&protected)
count=json.loads((b/'manifest.json').read_text())
count['kernel_release']=release
count['module_count']=len(modules)
count['disallowed_imports']=bad
count['duplicate_exports']=dup
count['config_sha256']=hashlib.sha256((o/'.config').read_bytes()).hexdigest()
(b/'manifest.json').write_text(json.dumps(count,indent=2)+'\n')
print('display-v11: staged %d modules for %s (depmod and GKI checks clean)'%(len(modules),release),flush=True)
for p in old:
    shutil.rmtree(p)
    print('display-v11: removed stale staging %s'%p.name,flush=True)

# ---- the GPU and CPU sets are flat directories of modules ----
for bundle in ('gpu-v7','cpu-v1'):
    b=w/'private'/bundle;modules=recorded(bundle)
    for name,path in sorted(modules.items()):install(fresh(name,path),b/name)
    manifest=json.loads((b/'manifest.json').read_text())
    manifest['kernel_release']=release
    (b/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('%s: refreshed %d modules for %s'%(bundle,len(modules),release),flush=True)
print('module caches refreshed for kernel release',release)
