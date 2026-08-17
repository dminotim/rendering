"""Read a text-serialised Unity scene and resolve every placed mesh to a world transform.

Unity splits a scene across three indirections and the layout only exists once all three are
followed:

  * The scene is almost entirely `PrefabInstance` records. Each names a source prefab and a list
    of *modifications* — `m_LocalPosition.x`, `m_LocalRotation.w`, and so on — which override
    properties of objects inside that prefab. The position of a chair is therefore not stored as
    a position but as four or seven scattered override entries.
  * The prefab itself holds the `MeshFilter` naming the mesh asset, plus its own local transform
    that the instance's overrides are applied on top of.
  * Both refer to assets by GUID, which only resolves through the package manifest.

Parent chains matter too: instances are nested under other instances and under plain transforms,
so a prop's world position is the product of everything above it.
"""
import yaml, re, os, sys, json, math

DOC = re.compile(r'^--- !u!(\d+) &(\d+)(.*)$', re.M)


def load_documents(path):
    """Splits a Unity YAML file into (classId, fileId, parsedBody) triples."""
    text = open(path, encoding='utf-8', errors='replace').read()
    marks = list(DOC.finditer(text))
    documents = []
    for i, mark in enumerate(marks):
        start = mark.end()
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        body = text[start:end]
        # `--- !u!114 &123 stripped` marks a component whose values live in the prefab; the body
        # is a stub and parsing it is still valid.
        try:
            parsed = yaml.safe_load(body)
        except yaml.YAMLError:
            parsed = None
        if isinstance(parsed, dict):
            documents.append((int(mark.group(1)), int(mark.group(2)), parsed))
    return documents


def quaternion_to_matrix(q, position, scale):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    if n < 1e-12:
        x, y, z, w = 0.0, 0.0, 0.0, 1.0
    else:
        x, y, z, w = x / n, y / n, z / n, w / n
    sx, sy, sz = scale
    # Column-major 4x4 as a flat list.
    return [
        (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0.0,
        (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0.0,
        (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0.0,
        position[0], position[1], position[2], 1.0,
    ]


def matmul(a, b):
    out = [0.0] * 16
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
    return out


IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def vec(d, key, default):
    v = d.get(key)
    if not isinstance(v, dict):
        return list(default)
    return [float(v.get('x', default[0])), float(v.get('y', default[1])),
            float(v.get('z', default[2]))]


def quat(d, key):
    v = d.get(key)
    if not isinstance(v, dict):
        return [0.0, 0.0, 0.0, 1.0]
    return [float(v.get('x', 0)), float(v.get('y', 0)),
            float(v.get('z', 0)), float(v.get('w', 1))]


class PrefabFile:
    """A prefab, reduced to the mesh it draws and the local transform it draws it at."""

    def __init__(self, path):
        self.path = path
        self.mesh_guid = None
        self.local = IDENTITY
        self.name = os.path.splitext(os.path.basename(path))[0]
        if not os.path.exists(path):
            return
        documents = load_documents(path)
        by_id = {fid: (cls, body) for cls, fid, body in documents}

        # The renderable is whichever GameObject carries a MeshFilter; a prefab in these packs has
        # exactly one, and its transform is the prefab root's unless the artist nested it.
        for cls, fid, body in documents:
            if cls != 33:
                continue
            filt = body.get('MeshFilter', {})
            mesh = filt.get('m_Mesh')
            if isinstance(mesh, dict) and mesh.get('guid'):
                self.mesh_guid = mesh['guid']
                owner = filt.get('m_GameObject', {}).get('fileID')
                self.local = self._transform_of(by_id, owner)
                break

    def _transform_of(self, by_id, game_object_id):
        for fid, (cls, body) in by_id.items():
            if cls != 4:
                continue
            t = body.get('Transform', {})
            if t.get('m_GameObject', {}).get('fileID') != game_object_id:
                continue
            local = quaternion_to_matrix(quat(t, 'm_LocalRotation'),
                                         vec(t, 'm_LocalPosition', (0, 0, 0)),
                                         vec(t, 'm_LocalScale', (1, 1, 1)))
            parent = t.get('m_Father', {}).get('fileID', 0)
            if parent:
                return matmul(self._transform_of_id(by_id, parent), local)
            return local
        return IDENTITY

    def _transform_of_id(self, by_id, transform_id):
        entry = by_id.get(transform_id)
        if not entry or entry[0] != 4:
            return IDENTITY
        t = entry[1].get('Transform', {})
        local = quaternion_to_matrix(quat(t, 'm_LocalRotation'),
                                     vec(t, 'm_LocalPosition', (0, 0, 0)),
                                     vec(t, 'm_LocalScale', (1, 1, 1)))
        parent = t.get('m_Father', {}).get('fileID', 0)
        if parent:
            return matmul(self._transform_of_id(by_id, parent), local)
        return local


def parse_scene(scene_path, manifest, project_root):
    documents = load_documents(scene_path)
    by_id = {fid: (cls, body) for cls, fid, body in documents}

    guid_to_path = {g: v['path'] for g, v in manifest.items()}
    prefab_cache = {}

    def prefab_for(guid):
        if guid not in prefab_cache:
            rel = guid_to_path.get(guid)
            prefab_cache[guid] = PrefabFile(os.path.join(project_root, rel)) if rel else None
        return prefab_cache[guid]

    # ── Plain transforms in the scene, for parent chains ──
    transforms = {}
    for cls, fid, body in documents:
        if cls != 4:
            continue
        t = body.get('Transform', {})
        transforms[fid] = {
            'local': quaternion_to_matrix(quat(t, 'm_LocalRotation'),
                                          vec(t, 'm_LocalPosition', (0, 0, 0)),
                                          vec(t, 'm_LocalScale', (1, 1, 1))),
            'parent': (t.get('m_Father') or {}).get('fileID', 0),
        }

    # ── Prefab instances, with their overrides applied ──
    instances = {}
    for cls, fid, body in documents:
        if cls != 1001:
            continue
        inst = body.get('PrefabInstance', {})
        mod = inst.get('m_Modification', {}) or {}
        source = (inst.get('m_SourcePrefab') or {}).get('guid')

        position = {'x': None, 'y': None, 'z': None}
        rotation = {'x': None, 'y': None, 'z': None, 'w': None}
        scale = {'x': None, 'y': None, 'z': None}
        name = None
        for entry in (mod.get('m_Modifications') or []):
            prop = entry.get('propertyPath', '')
            value = entry.get('value')
            if prop.startswith('m_LocalPosition.'):
                position[prop[-1]] = float(value)
            elif prop.startswith('m_LocalRotation.'):
                rotation[prop[-1]] = float(value)
            elif prop.startswith('m_LocalScale.'):
                scale[prop[-1]] = float(value)
            elif prop == 'm_Name':
                name = value

        instances[fid] = {
            'source': source,
            'parent': (mod.get('m_TransformParent') or {}).get('fileID', 0),
            'position': [position['x'] or 0.0, position['y'] or 0.0, position['z'] or 0.0],
            'rotation': [rotation['x'] or 0.0, rotation['y'] or 0.0,
                         rotation['z'] or 0.0, 1.0 if rotation['w'] is None else rotation['w']],
            'scale': [1.0 if scale[a] is None else scale[a] for a in 'xyz'],
            'name': name,
            'has_position': position['x'] is not None,
        }

    def world_of_transform(fid, depth=0):
        if not fid or depth > 32:
            return IDENTITY
        if fid in transforms:
            entry = transforms[fid]
            return matmul(world_of_transform(entry['parent'], depth + 1), entry['local'])
        # A transform id that belongs to a prefab instance resolves to that instance's world.
        if fid in instances:
            return world_of_instance(fid, depth + 1)
        return IDENTITY

    instance_world = {}

    def world_of_instance(fid, depth=0):
        if fid in instance_world:
            return instance_world[fid]
        if depth > 32:
            return IDENTITY
        inst = instances[fid]
        local = quaternion_to_matrix(inst['rotation'], inst['position'], inst['scale'])
        world = matmul(world_of_transform(inst['parent'], depth + 1), local)
        instance_world[fid] = world
        return world

    placements = []
    for fid, inst in instances.items():
        prefab = prefab_for(inst['source'])
        if prefab is None or prefab.mesh_guid is None:
            continue
        mesh_path = guid_to_path.get(prefab.mesh_guid)
        if not mesh_path:
            continue
        world = matmul(world_of_instance(fid), prefab.local)
        placements.append({
            'fileID': fid,
            'name': inst['name'] or prefab.name,
            'prefab': prefab.name,
            'mesh': mesh_path,
            'matrix': world,
        })
    return placements, instances, prefab_cache


if __name__ == '__main__':
    manifest = json.load(open(sys.argv[1], encoding='utf-8'))
    root = sys.argv[2]
    scene = sys.argv[3]
    placements, instances, prefabs = parse_scene(scene, manifest, root)
    print('prefab instances: %d' % len(instances))
    print('with resolved mesh: %d' % len(placements))
    import collections
    counts = collections.Counter(os.path.basename(p['mesh']) for p in placements)
    for mesh, n in counts.most_common(80):
        print('  %4d  %s' % (n, mesh))
    json.dump(placements, open(sys.argv[4], 'w', encoding='utf-8'), indent=1)
