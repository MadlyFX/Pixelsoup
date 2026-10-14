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
from mediaserver_livelink import translation
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

        # glTF importer shape: Base Color = Multiply(texture, baseColorFactor).
        tinted = bpy.data.materials.new('Imported glTF factor')
        tinted.use_nodes = True
        nodes, links = tinted.node_tree.nodes, tinted.node_tree.links
        bsdf = nodes.get('Principled BSDF')
        tex = nodes.new('ShaderNodeTexImage')
        tex.image = image
        mix = nodes.new('ShaderNodeMix')
        mix.data_type, mix.blend_type = 'RGBA', 'MULTIPLY'
        operands = {s.identifier: s for s in mix.inputs}
        operands['Factor_Float'].default_value = 1
        operands['B_Color'].default_value = (0.25, 0.5, 0.75, 1)
        links.new(tex.outputs['Color'], operands['A_Color'])
        links.new(mix.outputs['Result'], bsdf.inputs['Base Color'])
        descriptor = describe_material(tinted, ids, scene)
        assert descriptor['textures']['baseColorTexture']['path'] == str(Path(directory) / 'scan.png'), descriptor
        assert descriptor['factors'][:3] == [0.25, 0.5, 0.75], descriptor
        links.remove(operands['A_Color'].links[0])
        links.new(tex.outputs['Color'], operands['B_Color'])
        operands['A_Color'].default_value = (0.125, 0.375, 0.625, 1)
        assert describe_material(tinted, ids, scene)['factors'][:3] == [0.125, 0.375, 0.625]
        for blend, strength in (('ADD', 1), ('MULTIPLY', 0.5)):
            mix.blend_type, operands['Factor_Float'].default_value = blend, strength
            refused = describe_material(tinted, ids, scene)
            assert 'baseColorTexture' not in refused['textures'], refused
            assert max(abs(v - 0.8) for v in refused['factors'][:3]) < 1e-6, refused
        print('PASS: imported glTF base color factors multiply their texture')

        # The glTF importer names the render UV map on its Normal Map node.
        normal_tex = nodes.new('ShaderNodeTexImage')
        normal_tex.image = image
        normal_map = nodes.new('ShaderNodeNormalMap')
        normal_map.uv_map = 'Imported UV'
        normal_map.inputs['Strength'].default_value = 0.625
        links.new(normal_tex.outputs['Color'], normal_map.inputs['Color'])
        links.new(normal_map.outputs['Normal'], bsdf.inputs['Normal'])
        named = describe_material(tinted, ids, scene, 'Imported UV')['textures']
        assert named['normalTexture']['scale'] == 0.625, named
        assert 'normalTexture' not in describe_material(tinted, ids, scene, 'Other UV')['textures']
        assert 'normalTexture' not in describe_material(tinted, ids, scene)['textures']
        print('PASS: named UV maps are accepted only as the active render map')

        # The importer's alpha cutoff (Math nodes on the image alpha) keeps color.
        mix.blend_type, operands['Factor_Float'].default_value = 'MULTIPLY', 1
        cutoff = nodes.new('ShaderNodeMath')
        cutoff.operation = 'LESS_THAN'
        links.new(tex.outputs['Alpha'], cutoff.inputs[0])
        links.new(cutoff.outputs['Value'], bsdf.inputs['Alpha'])
        translation.notices.clear()
        cut = describe_material(tinted, ids, scene, 'Imported UV')
        assert cut['textures']['baseColorTexture']['path'] == str(Path(directory) / 'scan.png'), cut
        assert 'Imported glTF factor: ignored unassigned alpha texture' in translation.notices, translation.notices
        assert not any('direct Image Texture' in notice for notice in translation.notices), translation.notices
        print('PASS: an unsupported alpha graph keeps its color texture and says so accurately')

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
