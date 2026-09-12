"""Generate ptrcall signatures from the exact godot-cpp build API."""
import json
from pathlib import Path
import sys

api = json.loads(Path(sys.argv[1]).read_text())
# Variant type IDs are an ABI, including Nil (which has no builtin class).
names = ['Nil', 'bool', 'int', 'float', 'String', 'Vector2', 'Vector2i', 'Rect2',
         'Rect2i', 'Vector3', 'Vector3i', 'Transform2D', 'Vector4', 'Vector4i',
         'Plane', 'Quaternion', 'AABB', 'Basis', 'Transform3D', 'Projection',
         'Color', 'StringName', 'NodePath', 'RID', 'Object', 'Callable', 'Signal',
         'Dictionary', 'Array', 'PackedByteArray', 'PackedInt32Array', 'PackedInt64Array',
         'PackedFloat32Array', 'PackedFloat64Array', 'PackedStringArray', 'PackedVector2Array',
         'PackedVector3Array', 'PackedColorArray', 'PackedVector4Array']
ids = {name: i for i, name in enumerate(names)}
ids.update(Variant=-1, **{'void': -2})
methods, members, operators = [], [], []
operations = {"==": 0, "!=": 1, "<": 2, "<=": 3, ">": 4, ">=": 5,
              "+": 6, "-": 7, "*": 8, "/": 9, "%": 12, "**": 13,
              "<<": 14, ">>": 15, "&": 16, "|": 17, "^": 18,
              "~": 19, "and": 20, "or": 21, "xor": 22, "not": 23, "in": 24}
for cls in api['builtin_classes']:
    type_id = ids.get(cls['name'])
    if type_id is None:
        continue
    for method in cls.get('methods', []):
        args = method.get('arguments', [])
        result = ids.get(method.get('return_type', 'void'))
        if method['is_vararg'] or method['is_static'] or result is None or len(args) > 16:
            continue
        if any(a['type'] not in ids for a in args):
            continue
        types = ','.join(str(ids[a['type']]) for a in args) or '0'
        methods.append('{%d,%s,%du,%d,%d,{%s}}' %
                       (type_id, json.dumps(method['name']), method['hash'], result, len(args), types))
    for operator in cls.get('operators', []):
        right = ids.get(operator.get('right_type', 'Nil'))
        result = ids.get(operator['return_type'])
        op = operations.get(operator['name'])
        if 'right_type' not in operator and operator['name'] in ('-', '+'):
            op = 10 if operator['name'] == '-' else 11
        if op is not None and right is not None and right >= 0 and result is not None:
            operators.append('{%d,%d,%d,%d}' % (op, type_id, right, result))
    for member in cls.get('members', []):
        if member['type'] in ids:
            members.append('{%d,%s,%d}' % (type_id, json.dumps(member['name']), ids[member['type']]))
Path(sys.argv[2]).write_text('// Generated from the configured Godot API. Do not edit.\n'
    + 'static const BuiltinMethodInfo builtin_methods[] = {\n' + ',\n'.join(methods) + '\n};\n'
    + 'static const BuiltinMemberInfo builtin_members[] = {\n' + ',\n'.join(members) + '\n};\n'
    + 'static const BuiltinOperatorInfo builtin_operators[] = {\n' + ',\n'.join(operators) + '\n};\n')
