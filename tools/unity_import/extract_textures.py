"""Extract the needed textures from the package, downscaling them on the way out.

The package ships 4K 16-bit PNGs — 134 MB each, 2.25 GB in total. Unpacking those and handing them
to the renderer would spend more VRAM on textures than the whole rest of the scene costs, so each
one is decoded from the tar stream, resized, and written as a JPEG beside the FBX that uses it.
Nothing large ever lands on disk.

Output names follow the convention the FBX loader already looks for: `<meshStem>_BaseColor.jpg`
and `<meshStem>_Normal.jpg` in the mesh's own folder.
"""
import tarfile, sys, json, os, io
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

package = sys.argv[1]
manifest = json.load(open(sys.argv[2], encoding='utf-8'))
texmap = json.load(open(sys.argv[3], encoding='utf-8'))
asset_root = sys.argv[4]
max_size = int(sys.argv[5]) if len(sys.argv) > 5 else 2048

path_to_guid = {}
for guid, entry in manifest.items():
    path_to_guid.setdefault(entry['path'], guid)

# guid -> list of (output file, slot), because one texture is shared by several meshes.
targets = {}
SLOT_SUFFIX = {'base': 'BaseColor', 'normal': 'Normal'}
for mesh_base, entry in texmap.items():
    rel = entry['mesh'].replace('\\', '/')
    rel = rel.split('SaloonInterior/', 1)[-1]
    rel = rel.split('/', 1)[-1] if rel.startswith('HDRP') or rel.startswith('URP') else rel
    mesh_dir = os.path.join(asset_root, os.path.dirname(rel))
    stem = os.path.splitext(mesh_base)[0]
    for slot, png_path in entry['maps'].items():
        guid = path_to_guid.get(png_path)
        if not guid:
            continue
        out = os.path.join(mesh_dir, '%s_%s.jpg' % (stem, SLOT_SUFFIX[slot]))
        targets.setdefault(guid, []).append(out)

print('textures to extract: %d, output files: %d'
      % (len(targets), sum(len(v) for v in targets.values())))

done = 0
written_bytes = 0
with tarfile.open(package, 'r|gz') as tar:
    for member in tar:
        if not member.isfile():
            continue
        parts = member.name.split('/')
        if len(parts) < 2 or parts[-1] != 'asset':
            continue
        outputs = targets.get(parts[0])
        if not outputs:
            continue

        data = tar.extractfile(member).read()
        try:
            image = Image.open(io.BytesIO(data))
            image.load()
        except Exception as exc:
            print('  ! cannot decode %s: %s' % (outputs[0], exc))
            continue

        if max(image.size) > max_size:
            scale = max_size / float(max(image.size))
            image = image.resize((max(1, int(image.size[0] * scale)),
                                  max(1, int(image.size[1] * scale))),
                                 Image.LANCZOS)
        if image.mode not in ('RGB', 'L'):
            image = image.convert('RGB')

        for out in outputs:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            image.save(out, 'JPEG', quality=92, optimize=True)
            written_bytes += os.path.getsize(out)
        done += 1
        if done % 10 == 0:
            print('  ... %d/%d textures, %.0f MB written'
                  % (done, len(targets), written_bytes / 1e6), flush=True)

print('extracted %d of %d textures, %.1f MB on disk'
      % (done, len(targets), written_bytes / 1e6))
missing = [o for g, outs in targets.items() for o in outs if not os.path.exists(o)]
if missing:
    print('still missing %d outputs, first few:' % len(missing))
    for o in missing[:5]:
        print('   ', o)
