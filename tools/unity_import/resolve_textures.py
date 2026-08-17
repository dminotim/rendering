"""Work out which texture each mesh needs, following prefab -> material -> map.

Unity keeps none of this in the mesh: the prefab names a material, the material names textures, and
HDRP materials are often thin *instances* whose maps live on a parent asset. All three hops are
GUID-based, so the package manifest is the only thing that turns any of it into a filename.
"""
import os, re, sys, json, collections
import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unity_scene import load_documents

manifest = json.load(open(sys.argv[1], encoding='utf-8'))
asset_root = sys.argv[2]          # where Art/ was moved to
placements = json.load(open(sys.argv[3], encoding='utf-8'))
out_path = sys.argv[4]

guid_to_path = {g: v['path'] for g, v in manifest.items()}

# The .guid sidecars written during extraction map a GUID to where the file actually landed.
local_by_guid = {}
for dirpath, _, files in os.walk(asset_root):
    for name in files:
        if not name.endswith('.guid'):
            continue
        full = os.path.join(dirpath, name)
        guid = open(full, encoding='utf-8').read().strip()
        local_by_guid[guid] = full[:-5]

print('local files with guids: %d' % len(local_by_guid))

# The pack does not use one shader. Most props run a Quixel shader graph whose properties are
# `_Albedo`, `_Normal` and `_DR` (displacement + roughness packed together); some use stock HDRP
# `_BaseColorMap`/`_NormalMap`; a few use the built-in `_MainTex`; the layered-blend materials use
# `_BaseLayer*`. Each slot therefore lists every spelling, most preferred first.
MAP_KEYS = {
    'base': ('_Albedo', '_BaseColorMap', '_MainTex', '_BaseLayerAlbedoMap',
             '_BaseMap', '_AlbedoMap'),
    'normal': ('_Normal', '_NormalMap', '_BaseLayerNormalMap', '_BumpMap'),
}


def material_maps(guid, depth=0):
    """Candidate texture GUIDs per slot, in preference order.

    Returns every spelling that is present rather than just the best one. A material can name a
    texture the package never shipped — a layered floor material here points `_Albedo` at a GUID
    with no file behind it while its `_BaseLayerAlbedoMap` resolves fine — so the choice of which
    candidate to use cannot be made until the GUIDs are checked against the manifest. Deciding
    here would silently leave those meshes untextured.
    """
    path = local_by_guid.get(guid)
    if not path or not os.path.exists(path) or depth > 4:
        return {}
    text = open(path, encoding='utf-8', errors='replace').read()

    found = collections.defaultdict(list)
    # m_TexEnvs is a list of single-key maps; a regex is steadier here than YAML, because these
    # files carry duplicate keys and Unity-specific tags that safe_load rejects outright.
    for slot, keys in MAP_KEYS.items():
        for key in keys:
            pattern = re.compile(
                r'-\s+' + re.escape(key) + r':\s*\n\s+m_Texture:\s*\{fileID:\s*(-?\d+)'
                r'(?:,\s*guid:\s*([0-9a-zA-Z]+))?')
            match = pattern.search(text)
            if match and match.group(2):
                found[slot].append(match.group(2))

    parent = re.search(r'm_Parent:\s*\{fileID:\s*-?\d+,\s*guid:\s*([0-9a-zA-Z]+)', text)
    if parent:
        for slot, values in material_maps(parent.group(1), depth + 1).items():
            found[slot].extend(v for v in values if v not in found[slot])
    return dict(found)


def prefab_material_and_mesh(prefab_path):
    """(mesh guid, material guid) for the first MeshFilter/MeshRenderer pair in a prefab."""
    if not os.path.exists(prefab_path):
        return None, None
    text = open(prefab_path, encoding='utf-8', errors='replace').read()
    mesh = None
    # The first MeshFilter is LOD0; the collider's mesh sits on a MeshCollider (class 64) instead.
    m = re.search(r'--- !u!33 &\d+\n(?:.*\n)*?\s+m_Mesh:\s*\{fileID:\s*-?\d+,\s*guid:\s*([0-9a-zA-Z]+)',
                  text)
    if m:
        mesh = m.group(1)
    r = re.search(r'm_Materials:\s*\n\s+- \{fileID:\s*-?\d+,\s*guid:\s*([0-9a-zA-Z]+)', text)
    material = r.group(1) if r else None
    return mesh, material


# Which prefabs the saloon actually uses.
SALOON_PREFIXES = ('SM_His', 'SM_Res', 'SM_Ind', 'SM_Urb', 'SM_Mis')
used_meshes = {}
for p in placements:
    base = os.path.basename(p['mesh'])
    if base.startswith(SALOON_PREFIXES):
        used_meshes.setdefault(base, p['mesh'])

# Walk every prefab, keep the ones whose mesh is used.
result = {}
missing_material = []
prefab_dir = os.path.join(asset_root, 'Art', 'Prefabs')
for name in sorted(os.listdir(prefab_dir)):
    if not name.endswith('.prefab'):
        continue
    mesh_guid, material_guid = prefab_material_and_mesh(os.path.join(prefab_dir, name))
    if not mesh_guid:
        continue
    mesh_path = guid_to_path.get(mesh_guid, '')
    base = os.path.basename(mesh_path)
    if base not in used_meshes:
        continue
    if not material_guid:
        missing_material.append(base)
        continue
    maps = material_maps(material_guid)
    entry = result.setdefault(base, {'mesh': mesh_path, 'maps': {}})
    for slot, guids in maps.items():
        if slot in entry['maps']:
            continue
        # First candidate that the package actually ships a file for.
        for guid in guids:
            png = guid_to_path.get(guid)
            if png:
                entry['maps'][slot] = png
                break

json.dump(result, open(out_path, 'w', encoding='utf-8'), indent=1)

have = collections.Counter()
for base, entry in result.items():
    for slot in entry['maps']:
        have[slot] += 1
print('meshes used by the saloon: %d' % len(used_meshes))
print('meshes resolved to a material: %d' % len(result))
print('map coverage: %s' % dict(have))
without = [b for b in used_meshes if b not in result]
if without:
    print('no prefab/material found for %d meshes:' % len(without))
    for b in sorted(without):
        print('   ', b)
needed = {p for e in result.values() for p in e['maps'].values()}
print('distinct textures needed: %d' % len(needed))
json.dump(sorted(needed), open(out_path + '.textures', 'w', encoding='utf-8'), indent=1)
