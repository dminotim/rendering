# Importing a Unity asset pack as a `.dmscene`

Asset packs from Quixel and similar vendors ship two things that look like one: a **kit** (a folder
per prop, every one at the origin) and, separately, a **scene** that places them — which exists only
as a `.unitypackage`. The kit on its own cannot be rendered as anything but a contact sheet; the
layout is the part with the information in it.

These scripts pull the layout out and convert it. Run them in order.

```sh
# 1. Manifest: every asset's GUID -> project path. Streams the archive, unpacks nothing.
python unpack_manifest.py saloon.unitypackage manifest.json

# 2. Extract the layout-describing files only: scene, prefabs, meshes, materials.
#    Skips the textures, which are most of the archive.
python unpack_assets.py saloon.unitypackage manifest.json unpacked

#    Then move `unpacked/Assets/<vendor>/<pack>/<pipeline>/Art` somewhere with a SHORT path
#    (see "Path length" below) — e.g. E:/workspace/A/saloon_scene/Art.

# 3. Parse the scene into world-space placements.
python unity_scene.py manifest.json unpacked path/to/Scene.unity placements.json

# 4. Emit the .dmscene.
python export_dmscene.py placements.json <asset-root> out.dmscene <kit-root> saloon

# 5. Work out which texture each mesh needs...
python resolve_textures.py manifest.json <asset-root> placements.json texmap.json

# 6. ...and extract them, downscaled, straight from the archive.
python extract_textures.py saloon.unitypackage manifest.json texmap.json <asset-root> 2048
```

## Things that are not obvious and cost time

**A `.unitypackage` is a gzipped tar** of one directory per asset, named by GUID, each holding
`asset`, `asset.meta` and `pathname`. Nothing is addressed by name; every reference is a GUID, and
`pathname` is the only thing that turns a GUID back into something readable.

**The layout is three indirections deep.** The scene is almost entirely `PrefabInstance` records,
and a prop's position is not stored as a position — it is a list of *modifications* overriding
`m_LocalPosition.x`, `m_LocalRotation.w` and so on inside a referenced prefab. The prefab holds the
`MeshFilter` naming the mesh, plus its own local transform that the overrides compose with. Parent
chains nest instances under instances, so a world transform is a product up the whole chain.

**Handedness.** Unity is left-handed; FBX is right-handed, and Unity's importer negates Z on the way
in. Reading the FBX directly means every scene transform must be conjugated back: `M' = S M S` with
`S = diag(1, 1, -1)`. Getting this wrong does not produce nonsense — it produces a mirrored room
that looks fine until you notice the bar is on the wrong side.

**One FBX is not one mesh.** Game-ready exports carry LOD0–LOD3 *plus* a `ConvexHulls` body for the
physics engine, and the level is named on the FBX *Model* node, not on the geometry. Loading all of
them stacks four resolutions on top of each other and adds crude collision blocks that poke through
the surface. The loader filters on the model name; see `FbxLoader.cpp`.

**Texture property names are not standard.** Within one pack: `_Albedo`/`_Normal`/`_DR` on the
vendor's shader graph, `_BaseColorMap`/`_NormalMap` on stock HDRP, `_MainTex` on the built-in
shader, `_BaseLayer*` on layered blends. A material can also name a texture the package never
shipped, so a resolver must collect *every* spelling and pick the first whose GUID has a file behind
it — deciding on the first name that merely appears leaves those meshes untextured.

**Textures are 4K 16-bit PNGs**, 134 MB each and 2.25 GB in total for the pack this was written
against. `extract_textures.py` decodes them from the tar stream, resizes, and writes JPEGs beside
the mesh; nothing large lands on disk.

**Path length.** Windows still enforces `MAX_PATH` = 260, and `std::filesystem` honours it even
where Python's `os.path` quietly does not. The original project nesting
(`Assets/<vendor>/<pack>/HDRP (Default)/Art/Assets/MS/3D/<LongAssetName>/<LongMeshName>.fbx`) blows
past it, and the symptom is that the *longest-named* assets — and only those — come up "not found"
in the C++ loader while every Python check passes. Move the tree to a short root.

**The scene may contain more than the scene.** This pack's `.unity` holds the saloon *and* a Quixel
showroom — display plinths, logo panels, 10-metre wall tiles — overlapping it in space. They are
separated by asset-name family, not by position; see `SALOON_PREFIXES` in `export_dmscene.py`.
