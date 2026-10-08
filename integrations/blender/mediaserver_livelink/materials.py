"""Bounded Principled translation. RNA is inspected only by the Blender thread.

Descriptors contain paths and values, never RNA or file bytes. The serializer
resolves images on its worker and embeds them in immutable per-surface packages.
"""
import math

from .translation import bounded, warn

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
    def strict_image(socket, role, output='Color'):
        if not socket.is_linked:
            return False
        link = socket.links[0]
        node = link.from_node
        if node.type != 'TEX_IMAGE' or link.from_socket.name != output:
            refuse(role + ' requires a direct Image Texture; bake procedural color explicitly')
        source = node.image
        if source is None or source.source != 'FILE' or source.packed_file or source.is_dirty:
            refuse(role + ' requires a saved, unpacked PNG/JPEG image; save the image first')
        # Reading source.size can synchronously decode every 8K texture in an
        # imported scan. The worker validates dimensions and resizes export copies.
        expected = 'sRGB' if role in ('baseColorTexture', 'emissiveTexture') else 'Non-Color'
        if source.colorspace_settings.name != expected:
            warn(material.name + ': interpreting ' + role + ' as ' + expected)
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
    def image(socket, role, output='Color'):
        try:
            return strict_image(socket, role, output)
        except (ValueError, KeyError, AttributeError) as error:
            warn(str(error) + '; using material color without this texture')
            return False
    if not material.use_nodes:
        factors = [*material.diffuse_color[:3], material.roughness, material.metallic, 0, 0, 0]
        alpha, emission_strength, extensions = material.diffuse_color[3], 1, {}
    else:
        output = next((n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL' and n.is_active_output), None)
        shader = output.inputs['Surface'].links[0].from_node if output and len(output.inputs['Surface'].links) == 1 else None
        if shader is None or shader.type != 'BSDF_PRINCIPLED':
            warn(material.name + ': unsupported shader; using viewport material color')
            return {'id': identity, 'name': material.name,
                'factors': [*(bounded(v) for v in material.diffuse_color[:3]), bounded(material.roughness), bounded(material.metallic), 0, 0, 0],
                'textures': {}, 'alpha': bounded(material.diffuse_color[3], default=1),
                'alpha_mode': material.ms_link_alpha,
                'alpha_cutoff': material.ms_link_alpha_cutoff,
                'double_sided': not material.use_backface_culling, 'extensions': {}}
        if output.inputs['Volume'].is_linked or output.inputs['Displacement'].is_linked:
            warn(material.name + ': shader volume/displacement omitted; evaluated mesh retained')
        supported_links = {'Base Color', 'Roughness', 'Metallic', 'Normal', 'Emission Color', 'Alpha'}
        for socket in shader.inputs:
            if socket.is_linked and socket.name not in supported_links:
                warn(material.name + ': ignored linked ' + socket.name)
        for name, default in (('Subsurface Weight', 0), ('Sheen Weight', 0), ('Anisotropic', 0),
                              ('Thin Film Thickness', 0), ('Coat IOR', 1.5), ('Coat Tint', (1, 1, 1, 1)),
                              ('Specular Tint', (1, 1, 1, 1)), ('Weight', 1)):
            actual = value(shader, name, default)
            if isinstance(default, tuple):
                different = any(abs(a-b) > 1e-5 for a,b in zip(actual,default))
            else:
                different = abs(actual-default) > 1e-5
            if different:
                warn(material.name + ': approximated ' + name)
        base = list(value(shader, 'Base Color'))[:3]
        emission = list(value(shader, 'Emission Color'))[:3]
        if image(shader.inputs['Base Color'], 'baseColorTexture'):
            base = [1, 1, 1]
        if image(shader.inputs['Emission Color'], 'emissiveTexture'):
            emission = [1, 1, 1]
        roughness, metallic = value(shader, 'Roughness'), value(shader, 'Metallic')
        packed_node = None
        packed_valid = True
        for name, channel in (('Roughness', 'Green'), ('Metallic', 'Blue')):
            socket = shader.inputs[name]
            if not socket.is_linked:
                packed_valid = False
                continue
            link = socket.links[0]
            separate = link.from_node
            if separate.type != 'SEPARATE_COLOR' or separate.mode != 'RGB' or link.from_socket.name != channel:
                packed_valid = False
                warn(material.name + ': using constant ' + name.lower())
                continue
            if not image(separate.inputs['Color'], 'metallicRoughnessTexture'):
                packed_valid = False
                continue
            current = textures['metallicRoughnessTexture']
            if packed_node is not None and current != packed_node:
                packed_valid = False
            packed_node = current
        if packed_valid and packed_node is not None:
            roughness = metallic = 1
        else:
            textures.pop('metallicRoughnessTexture', None)
        normal = shader.inputs['Normal']
        if normal.is_linked:
            node = normal.links[0].from_node
            if (node.type == 'NORMAL_MAP' and node.space == 'TANGENT' and not node.uv_map
                    and not node.inputs['Strength'].is_linked
                    and image(node.inputs['Color'], 'normalTexture')):
                textures['normalTexture']['scale'] = bounded(node.inputs['Strength'].default_value, 0, 1, 1)
            else:
                warn(material.name + ': ignored unsupported normal texture')
        alpha = value(shader, 'Alpha', 1)
        if shader.inputs['Alpha'].is_linked:
            original = textures.get('baseColorTexture')
            if image(shader.inputs['Alpha'], 'baseColorTexture', 'Alpha') and original == textures.get('baseColorTexture'):
                alpha = 1
            else:
                if original is None:
                    textures.pop('baseColorTexture', None)
                else:
                    textures['baseColorTexture'] = original
                warn(material.name + ': ignored unassigned alpha texture')
        factors = [*base, roughness, metallic, *emission]
        emission_strength = value(shader, 'Emission Strength', 1)
        extensions = {
            'KHR_materials_emissive_strength': {'emissiveStrength': emission_strength},
            'KHR_materials_clearcoat': {'clearcoatFactor': value(shader, 'Coat Weight'),
                                       'clearcoatRoughnessFactor': value(shader, 'Coat Roughness')},
            'KHR_materials_transmission': {'transmissionFactor': value(shader, 'Transmission Weight')},
            'KHR_materials_ior': {'ior': value(shader, 'IOR', 1.5)},
            'KHR_materials_specular': {'specularFactor': 2 * value(shader, 'Specular IOR Level', 0.5)}}
    adjusted = [bounded(v) for v in factors]
    if adjusted != factors:
        warn(material.name + ': clamped material values for Media Server')
    factors = adjusted
    for extension, values in extensions.items():
        for key, number in values.items():
            low, high, default = (1, 3, 1.5) if key == 'ior' else (0, 1e6, 1) if key == 'emissiveStrength' else (0, 1, 0)
            values[key] = bounded(number, low, high, default)
    alpha = bounded(alpha, default=1)
    mode = material.ms_link_alpha
    # Opaque explicitly ignores alpha, including an imported texture's wired
    # alpha output. Promoting it to BLEND disables depth writes and makes solid
    # scans show their back surfaces in triangle/material submission order.
    return {'id': identity, 'name': material.name, 'factors': factors, 'textures': textures,
            'alpha': float(alpha), 'alpha_mode': mode, 'alpha_cutoff': material.ms_link_alpha_cutoff,
            'double_sided': not material.use_backface_culling, 'extensions': extensions}


def describe_light(obj, scale, efficacy):
    light = obj.data
    if light.type not in ('SUN', 'POINT', 'SPOT', 'AREA'):
        warn(obj.name + ': unsupported light ignored')
        return None
    node_color, node_strength = (1, 1, 1), 1
    if getattr(light, 'use_nodes', False):
        output = next((n for n in light.node_tree.nodes if n.type == 'OUTPUT_LIGHT' and n.is_active_output), None)
        emission = output.inputs['Surface'].links[0].from_node if output and len(output.inputs['Surface'].links) == 1 else None
        if emission and emission.type == 'EMISSION' and not any(i.is_linked for i in emission.inputs):
            node_color = emission.inputs['Color'].default_value[:3]
            node_strength = emission.inputs['Strength'].default_value
        else:
            warn(obj.name + ': unsupported light graph; using light color and power')
    if (not getattr(light, 'normalize', True) or getattr(light, 'use_temperature', False)
            or any(getattr(light, k, 1) != 1 for k in ('diffuse_factor', 'specular_factor', 'volume_factor'))
            or light.type in ('POINT', 'SPOT') and light.shadow_soft_size != 0
            or light.type == 'AREA' and light.shape not in ('SQUARE', 'RECTANGLE')):
        warn(obj.name + ': approximated light settings for Media Server')
    kind = {'SUN': 'directional', 'POINT': 'point', 'SPOT': 'spot', 'AREA': 'rectangle'}[light.type]
    power = bounded(light.energy * node_strength * efficacy, 0, 1e9)
    outer = bounded(light.spot_size / (4 * math.pi), 0.0001, 0.2489, 0.125) if light.type == 'SPOT' else 0.125
    values = [*(bounded(a*b) for a,b in zip(light.color, node_color)), power,
              bounded(obj.ms_link_light_range, 0.01, 10000, 20),
              outer * (1 - bounded(light.spot_blend)) if light.type == 'SPOT' else 0,
              outer,
              bounded(light.size * scale, 0.001, 10000, 1) if light.type == 'AREA' else 1,
              bounded((light.size_y if light.shape in ('RECTANGLE', 'ELLIPSE') else light.size) * scale,
                      0.001, 10000, 1) if light.type == 'AREA' else 1,
              bounded(light.angle / (4 * math.pi), 0, 0.2489) if light.type == 'SUN' else 0]
    return {'kind': kind, 'unit': 'lux' if kind == 'directional' else 'lumens', 'values': values}
