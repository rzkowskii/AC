#version 450
// Gamma (gfx->postpass(1)): the same as mslpost's fgamma in gfx_sdlgpu.cpp.
layout(location = 0) in vec2 tc;
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform PostU { vec4 params; }; // exponent
layout(location = 0) out vec4 ocolor;
void main()
{
    ocolor = vec4(pow(texture(tex, tc).rgb, vec3(params.x)), 1.0);
}
