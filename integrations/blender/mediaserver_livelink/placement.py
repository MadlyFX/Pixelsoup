"""Object placement in the terms the link uses: column-major matrices, local
transforms relative to the source parent, and source visibility."""
from mathutils import Matrix


def flatten(matrix):
    return [float(matrix[r][c]) for c in range(4) for r in range(4)]


def matrix(values):
    return Matrix(tuple(tuple(values[c * 4 + r] for c in range(4)) for r in range(4)))


def source_parent(obj):
    return obj.parent


def source_local(obj):
    return obj.parent.matrix_world.inverted() @ obj.matrix_world if obj.parent else obj.matrix_world.copy()


def source_visible(obj):
    return not obj.hide_render and obj.visible_get()
