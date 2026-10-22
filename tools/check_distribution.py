#!/usr/bin/env python3
"""Review source-only index and all reachable commit trees. Not legal clearance."""
import pathlib
import re
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parents[1]
TOP={'.gitignore','.clang-format','CMakeLists.txt','LICENSE','README.md','AGENTS.md'}
DIRS={'include','src','tests','tools','docs'}
EXTS={'.h','.hpp','.c','.cpp','.py','.md','.sh','.txt','.yml','.yaml'}
DENIED={'image_metadata.h','image.bin'}
checked=set()
errors=[]

def git(*args):
    return subprocess.check_output(['git',*args],cwd=ROOT)

def inspect(mode,oid,name,where):
    path=pathlib.PurePosixPath(name)
    permitted=name in TOP or (len(path.parts)>1 and path.parts[0] in DIRS and path.suffix in EXTS)
    if not permitted or path.name in DENIED or mode not in ('100644','100755'):
        errors.append(f'{where}: excluded path or mode: {name} ({mode})')
    if oid in checked:return
    checked.add(oid)
    data=git('cat-file','blob',oid)
    if len(data)>256*1024:errors.append(f'{where}: oversized source blob: {name}')
    try:text=data.decode('utf-8')
    except UnicodeError:errors.append(f'{where}: non-text blob: {name}');return
    if '\0' in text:errors.append(f'{where}: binary content: {name}')
    # Heuristics complement the path allowlist and manual source review.
    patterns=(r'/(?:Users|home)/[^/\s]+/',
              r'https?://[^\s]+(?:/-/raw/|/repository/(?:files|blobs))',
              r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----')
    if any(re.search(pattern,text) for pattern in patterns):
        errors.append(f'{where}: private/local/download material: {name}')

try:
    for entry in git('ls-files','--stage','-z').split(b'\0'):
        if not entry:continue
        meta,name=entry.decode().split('\t',1);mode,oid,stage=meta.split()
        if stage!='0':errors.append('Unresolved index entry: '+name)
        inspect(mode,oid,name,'index')
    for commit in git('rev-list','--all').decode().splitlines():
        for entry in git('ls-tree','-r','-z',commit).split(b'\0'):
            if not entry:continue
            meta,name=entry.decode().split('\t',1);mode,kind,oid=meta.split()
            if kind!='blob':errors.append(f'{commit}: non-blob entry: {name}');continue
            inspect(mode,oid,name,commit[:12])
    if errors:raise RuntimeError('\n'.join(errors))
    if not checked:raise RuntimeError('No staged or committed sources to check')
    print(f'Source-only distribution check passed: {len(checked)} unique blobs (index + all reachable commits)')
except (subprocess.CalledProcessError,RuntimeError) as exc:
    print(str(exc),file=sys.stderr);sys.exit(1)
