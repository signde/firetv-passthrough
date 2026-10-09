#!/usr/bin/env python3
"""Build the pinned source companion; source archives are not committed."""
import argparse, gzip, hashlib, json, tarfile, urllib.request
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cache-dir',type=Path,default=ROOT/'.cache/frida-sources')
args=parser.parse_args()
cache=args.cache_dir;cache.mkdir(parents=True,exist_ok=True)
items=json.loads((ROOT/'third_party/frida/source-manifest.json').read_text())
files=[]
for item in items:
    filename=item['archive']
    if Path(filename).name!=filename:raise ValueError('Invalid archive name')
    path=cache/filename
    if not path.exists():
        with urllib.request.urlopen(item['url'],timeout=60) as r:data=r.read()
        if hashlib.sha256(data).hexdigest()!=item['sha256']:raise ValueError(filename)
        path.write_bytes(data)
    if hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:raise ValueError(filename)
    files.append((path,'archives/'+filename))
for directory in ('third_party','licenses'):
    files.extend((p,p.relative_to(ROOT).as_posix()) for p in sorted((ROOT/directory).rglob('*')) if p.is_file())
files.append((ROOT/'tools/build_source_bundle.py','tools/build_source_bundle.py'))
files.append((ROOT/'LICENSE','LICENSE'))
out=ROOT/'dist/frida-17.22.2-source-and-notices.tar.gz';out.parent.mkdir(exist_ok=True)
with out.open('wb') as raw:
    with gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0,compresslevel=1) as compressed:
        with tarfile.open(fileobj=compressed,mode='w') as tar:
            for path,name in sorted(files,key=lambda x:x[1]):
                info=tar.gettarinfo(str(path),arcname=name)
                info.uid=info.gid=0;info.uname=info.gname='';info.mtime=0;info.mode=0o644
                with path.open('rb') as content:tar.addfile(info,content)
digest=hashlib.sha256(out.read_bytes()).hexdigest()
out.with_suffix(out.suffix+'.sha256').write_text(digest+'  '+out.name+'\n')
print(out);print('SHA256',digest);print('Source archives',len(items))
