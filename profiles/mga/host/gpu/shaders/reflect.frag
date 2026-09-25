#version 450

// Reflections on the floor, drawn at the seam between the world and the
// interface.
//
// Read this before turning it on
// ------------------------------
//
// This is the least certain of everything in this renderer, and the reasons
// are about the game rather than about the code.
//
// A screen-space reflection can only reflect what is already on the screen.
// In a game of corridors seen from above, most of what a floor ought to
// reflect -- the ceiling, the walls behind the camera -- was never drawn, so
// the ray runs off the edge of the picture and finds nothing. What is left is
// the near wall and whoever is standing on the floor, which is enough to read
// as a reflection and is not enough to be one.
//
// There is no material information anywhere in a PSP display list that says
// which surfaces are wet or polished. So instead of asking the material, this
// asks the geometry: it reflects only where the surface faces up. That makes
// it a floor reflection specifically, which is both the case it can do well
// and the case a stealth game actually wants. Walls, characters and props are
// left alone, whatever they are made of.
//
// World up is +Y. That is not a guess: the shadow map builds its light basis
// around {0, 1, 0} and falls back to straight overhead when a light dips below
// the floor, and that code produces shadows that land in the right place.
//
// The normals come from the derivatives of the reconstructed world position,
// because there is no normal buffer to read. That is exact on a flat surface
// and wrong wherever two surfaces meet in one quad -- every silhouette. The
// up-facing test hides most of that: a quad straddling the edge of a table
// produces a normal pointing somewhere sideways, which fails the test and is
// skipped. What it does not hide is a floor seen through a railing.
//
// One projection for the whole frame, the last 3D draw's, which is the same
// approximation the light shafts already run on.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

// A half-size copy of the finished world; the reflection is read out of it.
layout(set = 0, binding = 0) uniform sampler2D world_color;
// The depth the world left, at full size.
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

// 128 bytes exactly, which is the most a Vulkan implementation is required to
// offer. It holds one whole matrix and three quarters of another, and the
// reason three quarters is enough is worth stating, because carrying both in
// full would be 128 bytes with nothing left for the parameters.
//
// Reconstruction needs the whole inverse. Projection does not: the ray is a
// straight line, a projective transform takes straight lines to straight
// lines, and this pixel's own clip position is already known. So the only
// thing the forward matrix has to do is transform a DIRECTION, and a direction
// has w = 0, which never touches the matrix's fourth column.
layout(push_constant) uniform Push {
    mat4 clip_to_world;  // 0
    vec4 project_x;      // 64  -- the view-projection's first column
    vec4 project_y;      // 80
    vec4 project_z;      // 96
    // x: +1 when a larger depth value means nearer, -1 otherwise.
    // y: strength, 0 for off. z: how many steps the ray takes. w: spare.
    vec4 params;         // 112
} push;

// How far up a surface has to face before it counts as floor. cos(20 degrees):
// tight enough that a sloped wall is not a floor, loose enough that a
// derivative normal on a real floor, which is never quite exact, still passes.
const float kFloorFacing = 0.94;
// How far the ray reaches across the picture, in normalised device units. Two
// is the whole screen corner to corner, so one is a generous half.
const float kReach = 1.0;
// How far behind a surface the ray may be and still count as having hit it.
// Without a limit, a ray passing far behind a thin railing would report the
// railing as what it struck, and the floor would reflect things the railing
// was in front of rather than the railing itself.
const float kThickness = 0.012;

// Depth as 0 at the near plane and 1 at the far plane, whichever way round the
// GE left the buffer.
float distance_at(vec2 uv) {
    const float raw = texture(scene_depth, clamp(uv, 0.0, 1.0)).r;
    return push.params.x > 0.0 ? 1.0 - raw : raw;
}

// Screen position and raw depth to a world point.
vec3 world_at(vec2 uv, float raw_depth) {
    const vec4 clip = vec4(uv * 2.0 - 1.0, raw_depth, 1.0);
    const vec4 world = push.clip_to_world * clip;
    return world.xyz / (abs(world.w) > 1e-9 ? world.w : 1e-9);
}

void main() {
    const float strength = clamp(push.params.y, 0.0, 1.0);
    const float raw = texture(scene_depth, frag_uv).r;
    const vec3 here = world_at(frag_uv, raw);
    // Both derivatives taken before anything can branch: a derivative where
    // the quad does not all take the same path is undefined.
    const vec3 along_x = dFdx(here);
    const vec3 along_y = dFdy(here);

    const vec3 crossed = cross(along_x, along_y);
    const float extent = length(crossed);
    if (strength <= 0.0 || extent < 1e-12) {
        out_color = vec4(0.0);
        return;
    }
    vec3 normal = crossed / extent;
    // The cross product's sign depends on which way round the screen axes run
    // and on the handedness the game left in its projection, neither of which
    // this pass is in a position to know. A floor is the surface the camera is
    // above, so the normal that matters is the one pointing towards it.
    if (normal.y < 0.0) normal = -normal;
    if (normal.y < kFloorFacing) {
        out_color = vec4(0.0);
        return;
    }

    // The view direction, from the same reconstruction: the near plane under
    // this pixel, towards the surface.
    const vec3 eye = world_at(frag_uv, push.params.x > 0.0 ? 1.0 : 0.0);
    const vec3 view = normalize(here - eye);
    const vec3 ray = reflect(view, normal);
    // A ray heading into the floor it started on can only find the floor.
    if (dot(ray, normal) <= 0.0) {
        out_color = vec4(0.0);
        return;
    }

    // This pixel's clip position, rebuilt from what reconstruction already
    // worked out. clip = w * (ndc, 1), and the reconstruction's own w is 1/w.
    const vec4 unprojected = push.clip_to_world * vec4(frag_uv * 2.0 - 1.0, raw, 1.0);
    const float clip_w = 1.0 / (abs(unprojected.w) > 1e-9 ? unprojected.w : 1e-9);
    const vec4 clip_here = clip_w * vec4(frag_uv * 2.0 - 1.0, raw, 1.0);
    // The ray as a clip-space direction. w = 0, so no fourth column.
    const vec4 clip_ray = push.project_x * ray.x + push.project_y * ray.y + push.project_z * ray.z;

    // How far to travel, chosen so the ray covers kReach of the screen rather
    // than a fixed number of world units. This game's world units are in the
    // thousands and nothing here knows the scale, so a step measured on the
    // screen is the only one that means the same thing in every room.
    const float across = length(clip_ray.xy / max(abs(clip_w), 1e-6));
    const float reach = kReach / max(across, 1e-6);
    const int steps = int(clamp(push.params.z, 4.0, 48.0));

    vec2 hit_uv = vec2(0.0);
    float hit = 0.0;
    float travelled = 0.0;
    for (int i = 1; i <= steps; ++i) {
        const float t = reach * (float(i) / float(steps));
        const vec4 clip = clip_here + clip_ray * t;
        // Behind the eye: the ray has turned back past the camera and there is
        // nothing along it to find.
        if (clip.w <= 1e-6) break;
        const vec3 ndc = clip.xyz / clip.w;
        if (any(greaterThan(abs(ndc.xy), vec2(1.0)))) break;
        const vec2 uv = ndc.xy * 0.5 + 0.5;
        const float ray_distance = push.params.x > 0.0 ? 1.0 - ndc.z : ndc.z;
        const float surface = distance_at(uv);
        const float behind = ray_distance - surface;
        if (behind > 0.0 && behind < kThickness) {
            hit_uv = uv;
            hit = 1.0;
            travelled = float(i) / float(steps);
            break;
        }
    }
    if (hit <= 0.0) {
        out_color = vec4(0.0);
        return;
    }

    // Three fades, and each one hides a different way this can be wrong.
    //
    // At the edge of the picture the reflection simply runs out, and a hard
    // edge there is the single most obvious tell that reflections are being
    // done in screen space at all.
    const vec2 from_edge = abs(hit_uv * 2.0 - 1.0);
    const float edge = (1.0 - smoothstep(0.75, 1.0, from_edge.x)) * (1.0 - smoothstep(0.75, 1.0, from_edge.y));
    // A long ray has had more chances to pass behind something it should have
    // hit, so the further it went the less it is trusted.
    const float reach_fade = 1.0 - travelled * travelled * 0.6;
    // Fresnel: a floor seen from above reflects very little and a floor seen
    // along its length reflects a lot. Without this a reflection is equally
    // strong wherever you stand, which reads as a decal rather than a surface.
    const float grazing = pow(1.0 - clamp(dot(normal, -view), 0.0, 1.0), 4.0);
    const float weight = clamp(strength * edge * reach_fade * mix(0.08, 1.0, grazing), 0.0, 1.0);

    out_color = vec4(texture(world_color, hit_uv).rgb, weight);
}
