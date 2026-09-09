// Motion alone needs canonical interpolation for deforming triangles. Keep
// the extra triangle evaluation and flat payload out of color/shadow replays.
#define MODEL_MOTION 1
#include "scene.vert.hlsl"
