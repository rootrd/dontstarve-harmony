#version 450
layout(set=0,binding=0) uniform sampler2D source_image;
layout(push_constant) uniform Presentation { int conversion; } presentation;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    vec3 rgb = texture(source_image, uv).rgb;
    // Preserve the source's display-encoded bytes across unlike view formats.
    // sRGB sampling decodes; an UNORM swapchain does not encode on write.
    if (presentation.conversion == 1)
        rgb = mix(1.055 * pow(max(rgb, vec3(0)), vec3(1.0/2.4)) - 0.055,
                  12.92 * rgb, lessThanEqual(rgb, vec3(0.0031308)));
    else if (presentation.conversion == 2)
        rgb = mix(pow(max((rgb + 0.055) / 1.055, vec3(0)), vec3(2.4)),
                  rgb / 12.92, lessThanEqual(rgb, vec3(0.04045)));
    color = vec4(rgb, 1);
}
