#version 450
layout(location = 0) flat out uint instance;
void main()
{
   vec2 corners[6] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1),
                          vec2(0,1), vec2(1,0), vec2(1,1));
   uint cell = uint(gl_VertexIndex) / 6u;
   vec2 origin = vec2(cell % 8u, cell / 8u);
   origin.x += float(gl_InstanceIndex - 7) * 8.0;
   gl_Position = vec4((origin + corners[gl_VertexIndex % 6]) / 8.0 - 1.0, 0, 1);
   instance = uint(gl_InstanceIndex);
}
