#!/usr/bin/env bash
# Recompile GLSL -> SPIR-V -> C array. Run when you change any .vert/.frag
# in this folder; the output (../src/vk_shaders.c) is committed to git so
# downstream builds don't need glslangValidator installed.

set -e
cd "$(dirname "$0")"

glslangValidator -V quad.vert   -o quad.vert.spv
glslangValidator -V quad.frag   -o quad.frag.spv
glslangValidator -V world.vert  -o world.vert.spv
glslangValidator -V world.frag  -o world.frag.spv
glslangValidator -V world_warp.vert -o world_warp.vert.spv
glslangValidator -V world_warp.frag -o world_warp.frag.spv
glslangValidator -V entity.vert -o entity.vert.spv
glslangValidator -V entity.frag -o entity.frag.spv
glslangValidator -V entity_reflect.vert -o entity_reflect.vert.spv
glslangValidator -V entity_reflect.frag -o entity_reflect.frag.spv
glslangValidator -V part.vert   -o part.vert.spv
glslangValidator -V part.frag   -o part.frag.spv

python3 -c "
def dump(name, infile, header):
    with open(infile, 'rb') as f:
        data = f.read()
    words = len(data) // 4
    out = []
    if header:
        out.append('// Auto-generated from .vert/.frag files in src/ref_vk/shaders/')
        out.append('// Regenerate via src/ref_vk/shaders/shaders.sh')
        out.append('#include <stdint.h>')
        out.append('')
    out.append('const uint32_t %s_size = %d;' % (name, len(data)))
    out.append('const uint32_t %s_data[%d] = {' % (name, words))
    line = '    '
    for i in range(words):
        word = int.from_bytes(data[i*4:(i+1)*4], 'little')
        line += '0x%08x' % word
        if i + 1 < words: line += ', '
        if (i+1) % 6 == 0 or i+1 == words:
            out.append(line)
            line = '    '
    out.append('};')
    return '\n'.join(out) + '\n'

contents = dump('spirv_quad_vert',   'quad.vert.spv',   True)
contents += '\n' + dump('spirv_quad_frag',   'quad.frag.spv',   False)
contents += '\n' + dump('spirv_world_vert',  'world.vert.spv',  False)
contents += '\n' + dump('spirv_world_frag',  'world.frag.spv',  False)
contents += '\n' + dump('spirv_world_warp_vert', 'world_warp.vert.spv', False)
contents += '\n' + dump('spirv_world_warp_frag', 'world_warp.frag.spv', False)
contents += '\n' + dump('spirv_entity_vert', 'entity.vert.spv', False)
contents += '\n' + dump('spirv_entity_frag', 'entity.frag.spv', False)
contents += '\n' + dump('spirv_entity_reflect_vert', 'entity_reflect.vert.spv', False)
contents += '\n' + dump('spirv_entity_reflect_frag', 'entity_reflect.frag.spv', False)
contents += '\n' + dump('spirv_part_vert',   'part.vert.spv',   False)
contents += '\n' + dump('spirv_part_frag',   'part.frag.spv',   False)
with open('../src/vk_shaders.c', 'w') as f:
    f.write(contents)
"

rm -f *.spv
echo "Wrote $(pwd)/../src/vk_shaders.c"
