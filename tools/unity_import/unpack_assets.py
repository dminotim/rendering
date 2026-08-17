"""Pass 2: extract just the files that describe the layout, not the textures.

Scene, prefabs, meshes and materials together are a few hundred megabytes; the PNGs are the other
two and a half gigabytes and we already have equivalent maps on disk beside the kit.
"""
import tarfile, sys, json, os

src = sys.argv[1]
manifest = json.load(open(sys.argv[2], encoding='utf-8'))
outdir = sys.argv[3]
wanted_ext = set(sys.argv[4].split(',')) if len(sys.argv) > 4 else {'.unity', '.prefab', '.fbx', '.mat'}

keep = {g: v['path'] for g, v in manifest.items()
        if os.path.splitext(v['path'])[1].lower() in wanted_ext}
print('extracting %d assets' % len(keep))

written = 0
total = 0
with tarfile.open(src, 'r|gz') as tar:
    for member in tar:
        if not member.isfile():
            continue
        parts = member.name.split('/')
        if len(parts) < 2 or parts[-1] != 'asset':
            continue
        guid = parts[0]
        path = keep.get(guid)
        if path is None:
            continue
        # Mirror the project layout so relative references stay readable.
        target = os.path.join(outdir, path.replace('\\', '/'))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        data = tar.extractfile(member).read()
        with open(target, 'wb') as f:
            f.write(data)
        # A sidecar so the guid -> file mapping survives outside the manifest.
        with open(target + '.guid', 'w', encoding='utf-8') as f:
            f.write(guid)
        written += 1
        total += len(data)
        if written % 50 == 0:
            print('  ... %d files, %.0f MB' % (written, total / 1e6), flush=True)

print('wrote %d files, %.1f MB' % (written, total / 1e6))
