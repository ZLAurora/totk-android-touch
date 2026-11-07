#version 450
// The clear value's raw bits; the pipeline's colorWriteMask keeps the channels the clear must not touch.
// Compiled three times: CLEAR_TYPE 0 (float/normalized), 1 (unsigned integer), 2 (signed integer) attachments.
layout(push_constant) uniform Value { uvec4 bits; } value;
#if CLEAR_TYPE == 1
layout(location = 0) out uvec4 colour;
void main() { colour = value.bits; }
#elif CLEAR_TYPE == 2
layout(location = 0) out ivec4 colour;
void main() { colour = ivec4(value.bits); }
#else
layout(location = 0) out vec4 colour;
void main() { colour = uintBitsToFloat(value.bits); }
#endif
