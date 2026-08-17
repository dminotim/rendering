"""Pass 1 over a .unitypackage: build the guid -> project-path manifest.

A .unitypackage is a gzipped tar holding one directory per asset, named by GUID, containing
`asset` (the bytes), `asset.meta` (Unity's YAML sidecar) and `pathname` (where it lived in the
project). Only the manifest is collected here — the archive is mostly 4K textures we already have
on disk, and streaming past them is far cheaper than unpacking them.
"""
import tarfile, sys, json, os, collections

src = sys.argv[1]
out = sys.argv[2]

paths = {}
sizes = collections.Counter()
members = 0

with tarfile.open(src, 'r|gz') as tar:
    for member in tar:
        members += 1
        if members % 20000 == 0:
            print('  ... %d entries' % members, flush=True)
        if not member.isfile():
            continue
        parts = member.name.split('/')
        if len(parts) < 2:
            continue
        guid, leaf = parts[0], parts[-1]
        if leaf == 'pathname':
            data = tar.extractfile(member).read()
            paths[guid] = data.decode('utf-8', 'replace').split('\n')[0].strip()
        elif leaf == 'asset':
            sizes[guid] = member.size

manifest = {guid: {'path': path, 'size': sizes.get(guid, 0)} for guid, path in paths.items()}
with open(out, 'w', encoding='utf-8') as f:
    json.dump(manifest, f, indent=1)

print('entries %d, assets with pathname %d' % (members, len(manifest)))
by_ext = collections.Counter(os.path.splitext(v['path'])[1].lower() for v in manifest.values())
for ext, count in by_ext.most_common(20):
    print('  %-12s %d' % (ext or '(dir)', count))
print('--- scenes and prefabs ---')
for guid, v in sorted(manifest.items(), key=lambda kv: kv[1]['path']):
    if os.path.splitext(v['path'])[1].lower() in ('.unity', '.prefab'):
        print('  %s  %9d  %s' % (guid, v['size'], v['path']))
