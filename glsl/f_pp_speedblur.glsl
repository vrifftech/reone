#include "u_screeneffect.glsl"

uniform sampler2D sMainTex;
uniform sampler2D sLightmap;

noperspective in vec2 fragUV1;

out vec4 fragColor;

// The current frame (sMainTex) blended with the up-sampled history of the
// earlier output (sLightmap): history weighted by the ratio, the frame by the
// rest.
void main() {
    vec4 current = texture(sMainTex, fragUV1);
    vec3 history = texture(sLightmap, fragUV1).rgb;
    fragColor = vec4(mix(current.rgb, history, uSpeedBlurRatio), current.a);
}
