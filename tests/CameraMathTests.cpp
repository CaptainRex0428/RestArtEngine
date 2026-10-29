#include <RestArtEngine/Camera/Camera.h>
#include <RestArtEngine/Editor/CameraNavigation.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rae::camera;
int checks = 0;
void require(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
bool close(float a, float b, float tolerance = .0005f) { return std::abs(a-b) <= tolerance; }
std::array<float, 3> project(const View& view, const float* point) {
    const auto& m = view.viewProjection;
    const float w = point[0]*m[3]+point[1]*m[7]+point[2]*m[11]+m[15];
    return {(point[0]*m[0]+point[1]*m[4]+point[2]*m[8]+m[12])/w,
        (point[0]*m[1]+point[1]*m[5]+point[2]*m[9]+m[13])/w,
        (point[0]*m[2]+point[1]*m[6]+point[2]*m[10]+m[14])/w};
}
void projectionAndRays() {
    Camera camera;
    camera.lens.nearClip = 1; camera.lens.farClip = 21;
    camera.lens.verticalFovDegrees = 90; camera.lens.orthographicHeight = 4;
    for (auto projection : {Projection::Perspective, Projection::Orthographic}) {
        camera.lens.projection = projection;
        const auto view = evaluate(camera, 2);
        const float nearCenter[] = {0,0,-1}, farCenter[] = {0,0,-21};
        require(close(project(view, nearCenter)[2], 0), "near plane maps to canonical zero depth");
        require(close(project(view, farCenter)[2], 1), "far plane maps to canonical one depth");
        const auto center = screenRay(view, 0, 0);
        require(close(center.origin[2], -1) && close(center.direction[2], -1) && close(center.maxDistance,20),
            "center ray is clipped to near and far");
        const auto left = screenRay(view, -1, 0), right = screenRay(view, 1, 0);
        require(left.origin[0] < 0 && right.origin[0] > 0, "ray image handedness");
        require(projection == Projection::Orthographic ? close(left.direction[0],right.direction[0])
            : left.direction[0] < 0 && right.direction[0] > 0, "parallel versus diverging rays");
        for (float x : {-.8f,0.f,.8f}) for (float y : {-.6f,0.f,.6f}) {
            const auto ray = screenRay(view,x,y);
            float world[3];
            for (int i=0;i<3;++i) world[i]=ray.origin[i]+ray.direction[i]*3;
            const auto clip = project(view,world);
            require(close(clip[0],x) && close(clip[1],y), "ray and projection agree across image");
        }
    }
    camera.lens.aspectRatio = 1.5f;
    require(close(evaluate(camera,2).aspectRatio,1.5f), "authored aspect overrides viewport");
}
void poses() {
    const float target[] = {3,-2,8};
    for (float yaw : {-2.f,0.f,1.f}) for (float pitch : {-1.f,0.f,1.f}) {
        const auto camera = orbitCamera(yaw,pitch,7,target);
        const auto view = evaluate(camera,1.7f);
        const auto clip = project(view,target);
        require(close(clip[0],0) && close(clip[1],0), "orbit target remains centered");
        const auto ray = screenRay(view,0,0);
        for (int i=0;i<3;++i) require(close(ray.direction[i],view.forward[i]), "center ray matches pose forward");
    }
    Camera camera;
    camera.pose.rotation[1] = 90;
    require(close(evaluate(camera,1).forward[0],-1), "positive yaw faces negative world X");
    camera.pose.rotation[1] = 0; camera.pose.rotation[0] = 90;
    require(close(evaluate(camera,1).forward[1],1), "positive pitch faces positive Y without look-at singularity");
    camera.pose.rotation[0] = 0; camera.pose.rotation[2] = 90;
    require(close(evaluate(camera,1).up[0],-1), "roll rotates camera up");
}
void invalidInputs() {
    Lens lens;
    require(valid(lens), "default lens valid");
    lens.farClip = lens.nearClip;
    require(!valid(lens), "empty depth range rejected");
    lens = {}; lens.aspectRatio = -1;
    require(!valid(lens), "negative aspect rejected");
    lens = {}; lens.orthographicHeight = 0;
    require(!valid(lens), "zero ortho height rejected");
    lens = {}; lens.verticalFovDegrees = std::numeric_limits<float>::quiet_NaN();
    require(!valid(lens), "NaN rejected");
    lens = {}; lens.nearClip = 1e20f; lens.farClip = 2e20f;
    require(!valid(lens), "finite values overflowing the projection are rejected");
    lens = {}; lens.farClip = 1e7f;
    require(!valid(lens), "unrepresentable depth ratio rejected");
    lens = {}; lens.orthographicHeight = 1e30f; lens.aspectRatio = 1e30f;
    require(!valid(lens), "singular orthographic extents rejected");
    Camera camera; camera.pose.rotation[0] = std::numeric_limits<float>::infinity();
    require(!valid(camera), "infinite pose rejected");
    require(std::isfinite(evaluate(camera,0).viewProjection[0]), "render fallback remains finite");
}
void navigation() {
    using rae::editor::NavigationInput;
    using rae::editor::applyNavigation;
    using rae::renderer::SceneSettings;
    using rae::renderer::resolveCamera;
    SceneSettings top;
    top.cameraPose.emplace();
    top.cameraPose->position[1] = 12;
    top.cameraPose->rotation[0] = -90;
    top.cameraPose->rotation[2] = 27;
    // A preset may point exactly down. Translating or merely evaluating a fly
    // frame must not apply the rotation gesture's pole clamp or discard roll.
    for (int gesture = 0; gesture < 3; ++gesture) {
        auto settings = top;
        NavigationInput input; input.fly = true;
        if (gesture == 1) { input.panX = 18; input.panY = -9; }
        if (gesture == 2) input.dolly = 1;
        const auto before = evaluate(resolveCamera(settings), 1);
        applyNavigation(settings, input, 600);
        require(settings.cameraPose.has_value(), "fly frame retains an explicit camera pose");
        for (int i = 0; i < 3; ++i)
            require(settings.cameraPose->rotation[i] == top.cameraPose->rotation[i],
                "zero-input, pan and dolly preserve exact top-view orientation and roll");
        const auto after = evaluate(resolveCamera(settings), 1);
        float axialTravel = 0, totalTravel = 0;
        for (int i = 0; i < 3; ++i) {
            const float delta = after.position[i]-before.position[i];
            axialTravel += delta*before.forward[i]; totalTravel += delta*delta;
        }
        if (gesture == 0) require(totalTravel == 0, "zero-input fly frame leaves position unchanged");
        if (gesture == 1) require(totalTravel > 0 && close(axialTravel,0), "top-view pan stays in the image plane");
        if (gesture == 2) require(axialTravel > 0, "perspective dolly travels along top-view forward");
    }
    auto orthographic = top;
    orthographic.cameraLens.projection = Projection::Orthographic;
    orthographic.cameraLens.orthographicHeight = 8;
    NavigationInput zoom; zoom.dolly = 2;
    applyNavigation(orthographic, zoom, 600);
    require(orthographic.cameraPose.has_value() && orthographic.cameraLens.orthographicHeight < 8,
        "orthographic zoom changes image extent and retains pose");
    for (int i = 0; i < 3; ++i)
        require(orthographic.cameraPose->position[i] == top.cameraPose->position[i]
            && orthographic.cameraPose->rotation[i] == top.cameraPose->rotation[i],
            "orthographic dolly never moves or rotates the camera");
    SceneSettings orbit;
    const float originalYaw = orbit.cameraYaw, originalDistance = orbit.cameraDistance;
    NavigationInput turn; turn.orbitX = 15; turn.orbitY = -8; turn.panX = 4; turn.dolly = 1;
    applyNavigation(orbit, turn, 600);
    require(!orbit.cameraPose && orbit.cameraYaw != originalYaw && orbit.cameraDistance < originalDistance,
        "free orbit retains orbit representation while updating turn and distance");
    const auto pivot = project(evaluate(resolveCamera(orbit),1), orbit.cameraTarget);
    require(close(pivot[0],0) && close(pivot[1],0), "combined orbit, pan and dolly keep the current pivot centered");
    SceneSettings fly;
    fly.cameraPose.emplace();
    fly.cameraPose->position[0] = 3; fly.cameraPose->position[1] = -2; fly.cameraPose->position[2] = 9;
    fly.cameraPose->rotation[0] = 23; fly.cameraPose->rotation[1] = 47; fly.cameraPose->rotation[2] = -18;
    const auto flyBefore = evaluate(resolveCamera(fly),1);
    NavigationInput travel; travel.fly = true; travel.forward = 2.5f;
    applyNavigation(fly, travel, 600);
    require(fly.cameraPose.has_value(), "fly travel owns its resulting pose");
    for (int i = 0; i < 3; ++i)
        require(close(fly.cameraPose->position[i]-flyBefore.position[i], flyBefore.forward[i]*2.5f),
            "fly forward follows the current pitched and yawed camera basis");
}
void depthOfFieldData() {
    DepthOfField settings;
    require(!settings.enabled && settings.focusDistance == 5 && settings.fStop == 2.8f && settings.sensorHeightMm == 24,
        "depth of field defaults to disabled with explicit physical units");
    require(valid(settings), "default depth of field request is valid");
    for (float focus : {.01f, 1e6f}) for (float stop : {.7f,128.0f}) for (float sensor : {1.0f,100.0f}) {
        settings = {true, focus, stop, sensor};
        require(valid(settings), "documented depth of field boundary values are accepted");
    }
    for (bool enabled : {false,true}) for (int field = 0; field < 3; ++field) {
        for (float invalid : {0.0f, -1.0f, 1e30f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            settings = {}; settings.enabled = enabled;
            (field == 0 ? settings.focusDistance : field == 1 ? settings.fStop : settings.sensorHeightMm) = invalid;
            require(!valid(settings), "invalid authored DOF is rejected even while disabled");
            Lens lens; lens.depthOfField = settings;
            require(!valid(lens), "Lens validity includes its authored depth of field data");
        }
    }
    Camera camera;
    camera.pose.position[0] = 2; camera.pose.rotation[1] = 25;
    const auto pinhole = evaluate(camera,1.6f);
    const auto pinholeRay = screenRay(pinhole,.25f,-.4f);
    camera.lens.depthOfField = {true, .01f, .7f, 100};
    require(valid(camera), "focus before derived focal length preserves the camera; renderer decides effective DOF");
    const auto focused = evaluate(camera,1.6f);
    require(focused.viewProjection == pinhole.viewProjection && focused.inverseViewProjection == pinhole.inverseViewProjection,
        "postprocess depth of field never changes the geometric projection");
    const auto focusedRay = screenRay(focused,.25f,-.4f);
    for (int axis = 0; axis < 3; ++axis)
        require(focusedRay.origin[axis] == pinholeRay.origin[axis] && focusedRay.direction[axis] == pinholeRay.direction[axis],
            "geometric picking rays ignore postprocess lens blur");
    camera.lens.projection = Projection::Orthographic;
    require(valid(camera) && evaluate(camera,1).lens.depthOfField.enabled,
        "orthographic cameras preserve the DOF request for switching back to perspective");
}
}
int main() {
    try { projectionAndRays(); poses(); invalidInputs(); navigation(); depthOfFieldData(); std::cout << "Camera math: " << checks << " checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "Camera math failed: " << error.what() << '\n'; return 1; }
}
