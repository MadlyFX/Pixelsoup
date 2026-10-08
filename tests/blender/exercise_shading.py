"""Run with factory Blender: verify actual material RNA and evaluated tangents."""
from pathlib import Path
import sys
import tempfile

import bpy

repo = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repo / 'integrations/blender'))
sys.path.insert(0, str(repo / 'tests/blender'))
import mediaserver_livelink as addon
from mediaserver_livelink.extract import copy_mesh, Identities
from mediaserver_livelink.materials import describe_material
from test_shading_export import accessor
from geometry import glb

addon.register()
try:
    scene = bpy.context.scene
    ids = Identities()
    mat = bpy.data.materials.new('Opaque scan with an alpha socket')
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get('Principled BSDF')
    texture = mat.node_tree.nodes.new('ShaderNodeTexImage')
    with tempfile.TemporaryDirectory() as directory:
        image = bpy.data.images.new('Scan', width=2, height=2, alpha=True)
        image.pixels = [0.4, 0.6, 0.8, 0.25] * 4
        image.filepath_raw = str(Path(directory) / 'scan.png')
        image.file_format = 'PNG'
        image.save()
        image.reload()
        texture.image = image
        mat.node_tree.links.new(texture.outputs['Color'], shader.inputs['Base Color'])
        mat.node_tree.links.new(texture.outputs['Alpha'], shader.inputs['Alpha'])
        assert mat.ms_link_alpha == 'OPAQUE'
        for mode in ('OPAQUE', 'MASK', 'BLEND'):
            mat.ms_link_alpha = mode
            descriptor = describe_material(mat, ids, scene)
            assert descriptor['alpha_mode'] == mode, descriptor
            assert descriptor['alpha'] == 1
        mat.node_tree.links.remove(shader.inputs['Alpha'].links[0])
        shader.inputs['Alpha'].default_value = 0.3
        mat.ms_link_alpha = 'OPAQUE'
        assert describe_material(mat, ids, scene)['alpha_mode'] == 'OPAQUE'
        mat.ms_link_alpha = 'BLEND'
        assert abs(describe_material(mat, ids, scene)['alpha'] - 0.3) < 1e-6
        print('PASS: opaque, masked and blended materials retain explicit alpha mode')

    mesh = bpy.data.meshes.new('Mirrored UV tangent fixture')
    mesh.from_pydata([(0,0,0), (1,0,0), (0,1,0), (2,0,0), (3,0,0), (2,1,0)], [], [(0,1,2), (3,4,5)])
    uv = mesh.uv_layers.new(name='Render UV')
    uv.uv.foreach_set('vector', [0,0, 1,0, 0,1, 1,0, 0,0, 1,1])
    obj = bpy.data.objects.new('Mirrored UV fixture', mesh)
    scene.collection.objects.link(obj)
    bpy.context.view_layer.update()
    mesh.calc_tangents(uvmap=uv.name)
    expected = []
    for loop in mesh.loops:
        x, y, z = loop.tangent
        expected.extend((x, z, -y, loop.bitangent_sign))
    raw, materials = copy_mesh(obj.name, 1)
    assert len(raw) == 7
    _, data = glb(raw, materials)
    actual = accessor(data, 'TANGENT')
    assert len(actual) == len(expected) and max(abs(a-b) for a,b in zip(actual,expected)) < 1e-6
    assert set(actual[3::4]) == {-1, 1}
    print('PASS: evaluated mirrored-UV frames match Blender before and after GLB export')
finally:
    addon.unregister()
