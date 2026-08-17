"""Convert parsed Unity placements into a .dmscene the renderer can load.

Two conversions matter and both are easy to get silently wrong.

**Handedness.** Unity is left-handed (X right, Y up, Z into the screen); FBX is right-handed. When
Unity imports an FBX it negates the Z axis, so the transforms in the scene file act on
Z-negated geometry. Our loader reads the FBX directly and keeps it right-handed, so every scene
transform has to be conjugated back: M' = S M S with S = diag(1, 1, -1). Skipping this does not
produce nonsense — it produces a mirrored room that looks plausible until you notice the bar is on
the wrong side and every prop leans the wrong way.

**Units.** Unity works in metres and its FBX importer applies a 0.01 scale factor to centimetre
files. Our loader already converts to metres from `UnitScaleFactor`, so the geometry arrives in
metres too and the scene translations need no scaling — but the transform's *linear* part must not
be scaled either, which is why the conjugation is applied to the 3x3 alone.
"""
import json, os, sys, re, collections

placements = json.load(open(sys.argv[1], encoding='utf-8'))
project_root = sys.argv[2]
out_path = sys.argv[3]
kit_root = sys.argv[4]
group = sys.argv[5] if len(sys.argv) > 5 else 'saloon'

# The package ships a Quixel showroom in the same scene as the saloon: display plinths, logo
# panels, 10-metre wall tiles. They overlap the saloon in space, so they are separated by asset
# family rather than by position.
SALOON_PREFIXES = ('SM_His', 'SM_Res', 'SM_Ind', 'SM_Urb', 'SM_Mis')

def is_saloon(mesh_path):
    return os.path.basename(mesh_path).startswith(SALOON_PREFIXES)

def convert(matrix):
    """Conjugate a column-major 4x4 by diag(1,1,-1) and return the 3x4 the format wants."""
    m = matrix
    out = []
    for column in range(4):
        for row in range(3):
            value = m[column * 4 + row]
            # One sign flip for a Z row, one for a Z column; the ZZ entry gets both and cancels.
            if (row == 2) != (column == 2):
                value = -value
            out.append(value)
    return out

selected = [p for p in placements
            if (is_saloon(p['mesh']) if group == 'saloon'
                else (not is_saloon(p['mesh']) if group == 'showroom' else True))]

lines = [
    '# Historic Saloon, converted from the Unity scene shipped with the asset pack',
    '# (LV_Historic_Saloon.unity). Every line is one prefab instance with its world transform.',
    '#',
    '# Transforms are conjugated from Unity left-handed to the FBX right-handed convention the',
    '# meshes are stored in; see export_dmscene.py for why that is not optional.',
    'kit %s' % kit_root.replace('\\', '/'),
    '',
]

missing = collections.Counter()
written = 0
for p in sorted(selected, key=lambda q: (os.path.basename(q['mesh']), q['fileID'])):
    rel = p['mesh'].replace('\\', '/')
    # Strip the Unity project prefix. The files were moved to a short root because Windows
    # enforces a 260-character path limit that the original nesting blew straight past — and
    # std::filesystem honours it even where Python's os.path silently does not, so the C++ side
    # saw "file not found" for exactly the assets with the longest names.
    rel = re.sub(r'^Assets/Quixel/SaloonInterior/[^/]+/', '', rel)
    full = os.path.join(project_root, rel)
    if not os.path.exists(full):
        missing[os.path.basename(rel)] += 1
        continue
    values = convert(p['matrix'])
    lines.append('instance %s %s' % (rel, ' '.join('%.5f' % v for v in values)))
    written += 1

with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')

counts = collections.Counter(os.path.basename(p['mesh']) for p in selected)
print('group %-9s selected %d, written %d, distinct meshes %d'
      % (group, len(selected), written, len(counts)))
if missing:
    print('missing mesh files:')
    for name, n in missing.most_common():
        print('  %4d  %s' % (n, name))
