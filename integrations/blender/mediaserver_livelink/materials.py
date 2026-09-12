"""Bounded Principled translation. RNA is inspected only by the Blender thread.

Descriptors contain paths and values, never RNA or file bytes. The serializer
resolves images on its worker and embeds them in immutable per-surface packages.
"""
import math

DEFAULT = [0.8, 0.8, 0.8, 0.5, 0, 0, 0, 0]


def describe_material(material, identities, scene):
    if material is None:
        return list(DEFAULT)
    import bpy
    identity = identities.identity(material.original, scene)
    def refuse(message):
        raise ValueError(material.name + ': ' + message)
    textures = {}
    def value(shader, name, fallback=0):
        socket = shader.inputs.get(name)
        return socket.default_value if socket else fallback
    def image(socket, role, output='Color'):
        if not socket.is_linked:
            return False
        link = socket.links[0]
        node = link.from_node
        if node.type != 'TEX_IMAGE' or link.from_socket.name != output:
            refuse(role + ' requires a direct Image Texture; bake procedural color explicitly')
        source = node.image
        if source is None or source.source != 'FILE' or source.packed_file or source.is_dirty:
            refuse(role + ' requires a saved, unpacked PNG/JPEG image; save the image first')
        if max(source.size) > 4096 or min(source.size) < 1:
            refuse(role + ' image must be 1..4096 pixels per axis')
        expected = 'sRGB' if role in ('baseColorTexture', 'emissiveTexture') else 'Non-Color'
        if source.colorspace_settings.name != expected:
            refuse(role + ' image color space must be ' + expected)
        vector = node.inputs['Vector']
        if vector.is_linked:
            uv = vector.links[0]
            if not ((uv.from_node.type == 'UVMAP' and not uv.from_node.uv_map)
                    or (uv.from_node.type == 'TEX_COORD' and uv.from_socket.name == 'UV')):
                refuse('textures require the active render UV map without Mapping/texture transforms')
        if node.projection != 'FLAT' or node.extension not in ('REPEAT', 'EXTEND') or node.interpolation not in ('Linear', 'Closest'):
            refuse('use Flat projection, Repeat/Extend and Linear/Closest sampling')
        textures[role] = {'path': bpy.path.abspath(source.filepath, library=source.library),
                          'wrap': 10497 if node.extension == 'REPEAT' else 33071,
                          'filter': 9729 if node.interpolation == 'Linear' else 9728}
        return True
    if not material.use_nodes:
        factors = [*material.diffuse_color[:3], material.roughness, material.metallic, 0, 0, 0]
        alpha, emission_strength, extensions = material.diffuse_color[3], 1, {}
    else:
        output = next((n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL' and n.is_active_output), None)
        if not output or len(output.inputs['Surface'].links) != 1:
            refuse('requires one Principled BSDF connected to Material Output')
        shader = output.inputs['Surface'].links[0].from_node
        if shader.type != 'BSDF_PRINCIPLED' or output.inputs['Volume'].is_linked or output.inputs['Displacement'].is_linked:
            refuse('shader mixtures, volumes and shader displacement require evaluated geometry or explicit baking')
        supported_links = {'Base Color', 'Roughness', 'Metallic', 'Normal', 'Emission Color', 'Alpha'}
        for socket in shader.inputs:
            if socket.is_linked and socket.name not in supported_links:
                refuse(socket.name + ' does not support linked inputs')
        for name, default in (('Subsurface Weight', 0), ('Sheen Weight', 0), ('Anisotropic', 0),
                              ('Thin Film Thickness', 0), ('Coat IOR', 1.5), ('Coat Tint', (1, 1, 1, 1)),
                              ('Specular Tint', (1, 1, 1, 1)), ('Weight', 1)):
            actual = value(shader, name, default)
            if isinstance(default, tuple):
                different = any(abs(a-b) > 1e-5 for a,b in zip(actual,default))
            else:
                different = abs(actual-default) > 1e-5
            if different:
                refuse(name + ' is outside the supported look contract')
        base = list(value(shader, 'Base Color'))[:3]
        emission = list(value(shader, 'Emission Color'))[:3]
        if image(shader.inputs['Base Color'], 'baseColorTexture'):
            base = [1, 1, 1]
        if image(shader.inputs['Emission Color'], 'emissiveTexture'):
            emission = [1, 1, 1]
        roughness, metallic = value(shader, 'Roughness'), value(shader, 'Metallic')
        packed_node = None
        for name, channel in (('Roughness', 'Green'), ('Metallic', 'Blue')):
            socket = shader.inputs[name]
            if not socket.is_linked:
                continue
            link = socket.links[0]
            separate = link.from_node
            if separate.type != 'SEPARATE_COLOR' or separate.mode != 'RGB' or link.from_socket.name != channel:
                refuse(name + ' requires Separate Color RGB: green roughness, blue metallic')
            if not image(separate.inputs['Color'], 'metallicRoughnessTexture'):
                refuse('packed roughness/metallic requires an image')
            current = textures['metallicRoughnessTexture']
            if packed_node is not None and current != packed_node:
                refuse('roughness and metallic must use the same packed image and sampler')
            packed_node = current
        if packed_node is not None:
            if not all(shader.inputs[n].is_linked for n in ('Roughness', 'Metallic')):
                refuse('connect both packed roughness and metallic channels')
            roughness = metallic = 1
        normal = shader.inputs['Normal']
        if normal.is_linked:
            node = normal.links[0].from_node
            if node.type != 'NORMAL_MAP' or node.space != 'TANGENT' or node.uv_map or node.inputs['Strength'].is_linked:
                refuse('normal requires tangent Normal Map using the active render UV map')
            if not image(node.inputs['Color'], 'normalTexture'):
                refuse('Normal Map requires a Non-Color image')
            textures['normalTexture']['scale'] = float(node.inputs['Strength'].default_value)
        alpha = value(shader, 'Alpha', 1)
        if shader.inputs['Alpha'].is_linked:
            base_image = textures.get('baseColorTexture')
            image(shader.inputs['Alpha'], 'baseColorTexture', 'Alpha')
            if base_image != textures['baseColorTexture']:
                refuse('alpha must come from the base color image')
            alpha = 1
        factors = [*base, roughness, metallic, *emission]
        emission_strength = value(shader, 'Emission Strength', 1)
        extensions = {
            'KHR_materials_emissive_strength': {'emissiveStrength': emission_strength},
            'KHR_materials_clearcoat': {'clearcoatFactor': value(shader, 'Coat Weight'),
                                       'clearcoatRoughnessFactor': value(shader, 'Coat Roughness')},
            'KHR_materials_transmission': {'transmissionFactor': value(shader, 'Transmission Weight')},
            'KHR_materials_ior': {'ior': value(shader, 'IOR', 1.5)},
            'KHR_materials_specular': {'specularFactor': 2 * value(shader, 'Specular IOR Level', 0.5)}}
    if not all(math.isfinite(v) and 0 <= v <= 1 for v in factors) or not math.isfinite(emission_strength) or not 0 <= emission_strength <= 1e6:
        refuse('material factors must be finite in 0..1; emission strength in 0..1000000')
    mode = material.ms_link_alpha
    if mode == 'OPAQUE' and (alpha != 1 or (material.use_nodes and shader.inputs['Alpha'].is_linked)):
        refuse('select Mask or Blend in the Media Server alpha control')
    return {'id': identity, 'name': material.name, 'factors': factors, 'textures': textures,
            'alpha': float(alpha), 'alpha_mode': mode, 'alpha_cutoff': material.ms_link_alpha_cutoff,
            'double_sided': not material.use_backface_culling, 'extensions': extensions}


def describe_light(obj, scale, efficacy):
    light = obj.data
    def refuse(message):
        raise ValueError(obj.name + ': ' + message)
    if light.type not in ('SUN', 'POINT', 'SPOT', 'AREA') or (light.type == 'AREA' and light.shape not in ('SQUARE', 'RECTANGLE')):
        refuse('supported lights: Sun, Point, Spot and square/rectangle Area')
    if not getattr(light, 'normalize', True):
        refuse('enable Normalize for explicit radiometric power')
    node_color, node_strength = (1,1,1), 1
    if getattr(light,'use_nodes',False):
        output = next((n for n in light.node_tree.nodes if n.type=='OUTPUT_LIGHT' and n.is_active_output),None)
        emission = output.inputs['Surface'].links[0].from_node if output and len(output.inputs['Surface'].links)==1 else None
        if not emission or emission.type!='EMISSION' or any(i.is_linked for i in emission.inputs):
            refuse('light graphs require one constant Emission connected to Light Output')
        node_color = emission.inputs['Color'].default_value[:3]
        node_strength = emission.inputs['Strength'].default_value
    if any(getattr(light,k,1)!=1 for k in ('diffuse_factor','specular_factor','volume_factor')):
        refuse('light contribution factors must remain one')
    if getattr(light, 'use_temperature', False):
        refuse('disable Blender temperature and author linear RGB light color')
    if light.type in ('POINT', 'SPOT') and light.shadow_soft_size != 0:
        refuse('Point/Spot radius must be zero; use a rectangle for a finite source')
    kind = {'SUN': 'directional', 'POINT': 'point', 'SPOT': 'spot', 'AREA': 'rectangle'}[light.type]
    power = light.energy * node_strength * efficacy
    # Explicit user-selected radiometric-to-photometric approximation, not a
    # spectral reconstruction of an RGB light. Normalize makes watts unambiguous.
    values = [*(a*b for a,b in zip(light.color,node_color)), power, obj.ms_link_light_range,
              light.spot_size / (4*math.pi) * (1-light.spot_blend) if light.type == 'SPOT' else 0,
              light.spot_size / (4*math.pi) if light.type == 'SPOT' else 0.125,
              light.size * scale if light.type == 'AREA' else 1,
              (light.size_y if light.shape == 'RECTANGLE' else light.size)*scale if light.type == 'AREA' else 1,
              light.angle/(4*math.pi) if light.type == 'SUN' else 0]
    if not all(math.isfinite(v) and 0 <= v <= 1e9 for v in values):
        refuse('light values must be finite and nonnegative')
    return {'kind': kind, 'unit': 'lux' if kind == 'directional' else 'lumens', 'values': values}
