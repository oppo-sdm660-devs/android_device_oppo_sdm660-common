#!/usr/bin/env python3
"""Build a camera source snapshot with commands exported by its existing Soong build.

This verifies native compilation/linking; it does not regenerate Soong or build a ROM.
All camera sources and module objects stay in the snapshot directory.
"""
import argparse
import concurrent.futures
import pathlib
import re
import shlex
import subprocess
import sys

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-top', type=pathlib.Path, required=True)
parser.add_argument('--snapshot', type=pathlib.Path, required=True, help='Directory containing camera/ and commands.txt')
parser.add_argument('--ninja-file', type=pathlib.Path, required=True, help='Target incremental ninja file, relative to build top')
parser.add_argument('--jobs', type=int, default=6)
parser.add_argument('--module', choices=('camera.sdm660', 'libmmcamera_interface'), default='camera.sdm660')
options=parser.parse_args()
build_top=options.build_top.resolve()
overlay=options.snapshot.resolve()
source_prefix='device/oppo/sdm660-common/camera/'
module_path = 'QCamera2/stack/' if options.module == 'libmmcamera_interface' else ''
obj_prefix=('out/soong/.intermediates/device/oppo/sdm660-common/camera/' +
            module_path + options.module + '/android_vendor_arm_armv8-a_shared/')
final=obj_prefix+'unstripped/'+options.module+'.so'
commands=(overlay/'commands.txt').read_text().splitlines()
rows=[r for r in commands if re.search(r'/clang(?:\+\+)? -c ', r) and obj_prefix in r]
if not rows:sys.exit('No compiler commands for '+options.module)
# Preserve the actual Soong flags while compiling newly added module sources too.
compiled={shlex.split(r)[-1] for r in rows}
bp=(overlay/'camera'/module_path/'Android.bp').read_text()
start=bp.index('    srcs: [',bp.index('name: "'+options.module+'"'))
end=bp.index('    ],',start)
sources=re.findall(r'"([^"\n]+\.cpp)"',bp[start:end]) if not module_path else []
base=next((r for r in rows if r.endswith('QCamera3OppoPanorama.cpp')), None)
for source in sources:
    original=source_prefix+source
    if original not in compiled:
        rows.append(base.replace('QCamera2/HAL3/QCamera3OppoPanorama.cpp',source)
                    .replace('QCamera2/HAL3/QCamera3OppoPanorama.o',source[:-4]+'.o'))


def adapt(row):
    result=[]
    for arg in shlex.split(row):
        if arg.startswith(obj_prefix):arg=str(overlay/'obj')+'/'+arg[len(obj_prefix):]
        elif arg.startswith('@'+obj_prefix):arg='@'+str(overlay/'obj')+'/'+arg[len(obj_prefix)+1:]
        elif arg.startswith(source_prefix):arg=str(overlay/'camera')+'/'+arg[len(source_prefix):]
        elif arg.startswith('-I'+source_prefix):arg='-I'+str(overlay/'camera')+'/'+arg[len(source_prefix)+2:]
        result.append(arg)
    return shlex.join(result)

def compile_one(row):
    row=adapt(row)
    args=shlex.split(row)
    if args and args[0].startswith('PWD='):args.pop(0)
    for flag in ('-o','-MF'):
        if flag in args:pathlib.Path(args[args.index(flag)+1]).parent.mkdir(parents=True,exist_ok=True)
    result=subprocess.run(args,cwd=build_top,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    name=args[-1]
    return result.returncode,name,result.stdout

failures=0
with concurrent.futures.ThreadPoolExecutor(max_workers=options.jobs) as pool:
    for rc,name,output in pool.map(compile_one,rows):
        print(('FAIL' if rc else 'PASS'),name,flush=True)
        if output: print(output,flush=True)
        failures+=rc!=0
print('COMPILE',len(rows),'failures',failures,flush=True)
if failures:sys.exit(1)
links=[r for r in commands if 'clang++' in r and '-shared' in r and final in r]
if not links:sys.exit('No linker command')
row=adapt(links[-1])
args=shlex.split(row)
# All module objects compiled above; linker response file also contains object paths.
for i,arg in enumerate(args):
    if arg.startswith('@'):
        original=arg[1:].replace(str(overlay/'obj')+'/',obj_prefix)
        source=build_top/original
        if source.exists():rsp=source.read_text()
        else:
            ninja=(build_top/options.ninja_file).read_text()
            ninja=ninja.replace('$\\n',' ').replace('$\n',' ')
            rule=re.search(r'^build\s+'+re.escape(final)+r':\s+\S+\s+([^\n]+)',ninja,re.M)
            if rule is None:raise RuntimeError('cannot find linker rule')
            rsp=rule.group(1).split('|')[0].strip()
        for row in rows:
            object_path=shlex.split(row)[shlex.split(row).index('-o')+1]
            if object_path not in rsp:rsp+=' '+object_path
        rsp=rsp.replace(obj_prefix,str(overlay/'obj')+'/')
        path=overlay/(options.module+'.rsp');path.write_text(rsp);args[i]='@'+str(path)
if args[0].startswith('PWD='):args.pop(0)
pathlib.Path(args[args.index('-o')+1]).parent.mkdir(parents=True,exist_ok=True)
result=subprocess.run(args,cwd=build_top)
print('LINK',result.returncode,flush=True)
sys.exit(result.returncode)
