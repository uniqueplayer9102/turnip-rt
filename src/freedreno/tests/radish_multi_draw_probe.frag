#version 450
layout(set = 0, binding = 0) uniform Colors { vec4 value; } colors;
layout(location = 0) flat in uint instance;
layout(location = 0) out vec4 color;
void main()
{
   color = instance == 7u ? colors.value : colors.value.bgra;
}
