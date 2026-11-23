#version 450
layout(location = 0) out vec4 color;
layout(constant_id = 0) const uint pattern_phase = 0u;
void main()
{
   uint x = uint(gl_FragCoord.x);
   uint y = uint(gl_FragCoord.y);
   color = vec4(float(((x / 17u) + pattern_phase) & 1u),
                float(((y / 29u) + pattern_phase) & 1u),
                float((((x + y) / 31u) + pattern_phase) & 1u), 1);
}
