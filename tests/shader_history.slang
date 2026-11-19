#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = global.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
layout(set = 0, binding = 3) uniform sampler2D OriginalHistory1;
void main() { FragColor = vec4((texture(Source, uv).rgb + texture(OriginalHistory1, uv).rgb) * 0.5, 1.0); }
