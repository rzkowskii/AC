#version 450
// The GPU backend's post-processing passes for Vulkan (gamma, FXAA), in GLSL 450: the same as mslpost in gfx_sdlgpu.cpp
// (keep them in step). A full-screen triangle; "GL" coordinates have their origin at the bottom left like the GL
// versions in shaders.cpp, so FXAA's sample pattern is the same.
layout(location = 0) out vec2 tc;
void main()
{
    uint vid = uint(gl_VertexIndex);
    vec2 p = vec2(float((vid << 1u) & 2u), float(vid & 2u));
    gl_Position = vec4(p*2.0 - 1.0, 0.0, 1.0);
    tc = vec2(p.x, 1.0 - p.y);
}
