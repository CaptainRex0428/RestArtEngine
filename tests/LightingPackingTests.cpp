#include <RestArtEngine/Renderer/Lighting.h>
#include <RestArtEngine/Scene/World.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace rae::scene;
using namespace rae::renderer;
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a - b) <= tolerance; }
Actor lightActor(LightType type) {
    Actor actor;
    actor.mesh = {0, 0};
    actor.light = LightComponent{};
    actor.light->type = type;
    return actor;
}
void transformsAndTypes() {
    for (auto type : {LightType::Directional, LightType::Sky, LightType::Rect, LightType::Spot, LightType::Point}) {
        World world;
        world.actors.push_back(lightActor(type));
        auto& actor = world.actors.front();
        actor.transform.position[0] = 3;
        actor.transform.position[1] = -2;
        actor.transform.position[2] = 7;
        actor.transform.rotation[1] = 90;
        const auto unitScale = gatherLighting(world, 100);
        actor.transform.scale[0] = -4;
        actor.transform.scale[1] = 0;
        actor.transform.scale[2] = 13;
        const auto scaled = gatherLighting(world, 100);
        require(unitScale == scaled, std::string(lightTypeName(type)) + ": scale must not change lighting");
        require(scaled.info[1] == 1, "all five types select authored lighting");
        require(scaled.sky[3] == 1, "actor-authored flag distinguishes the legacy PT emitter contract");
        if (type == LightType::Sky) {
            require(scaled.info[0] == 0 && scaled.sky[0] == 10 && scaled.sky[1] == 10 && scaled.sky[2] == 10,
                "sky aggregates radiance without a direct-light slot");
            continue;
        }
        const auto& light = scaled.lights[0];
        require(scaled.info[0] == 1 && light.positionType[3] == static_cast<float>(type), "direct type reaches GPU");
        require(light.positionType[0] == 3 && light.positionType[1] == -2 && light.positionType[2] == 7, "position reaches GPU");
        require(near(light.directionRange[0], -1) && near(light.directionRange[1], 0) && near(light.directionRange[2], 0),
            "yaw +90 aims local -Z toward world -X");
        require(near(light.axisUWidth[0], 0) && near(light.axisUWidth[2], -1) && near(light.axisVHeight[1], 1),
            "rectangle axes rotate with the light");
        actor.transform.rotation[1] = 0;
        actor.transform.rotation[0] = 90;
        const auto pitched = gatherLighting(world, 100);
        require(near(pitched.lights[0].directionRange[1], 1), "pitch +90 aims toward world +Y");
    }
}
void budgetAndVisibility() {
    World world;
    for (std::uint32_t i = 0; i < maxSceneLights + 3; ++i) {
        auto actor = lightActor(LightType::Point);
        actor.transform.position[0] = static_cast<float>(i);
        world.actors.push_back(actor);
    }
    auto sky = lightActor(LightType::Sky);
    sky.light->intensity = 2;
    sky.light->color[0] = .5f;
    world.actors.insert(world.actors.begin(), sky);
    sky.light->color[1] = .25f;
    world.actors.push_back(sky);
    const auto packed = gatherLighting(world, 5);
    require(packed.info[0] == maxSceneLights && packed.info[3] == 3, "overflow is reported without changing the GPU budget");
    require(packed.lights[0].positionType[0] == 0 && packed.lights[maxSceneLights - 1].positionType[0] == maxSceneLights - 1,
        "direct-light budget follows stable authored order");
    require(packed.sky[0] == 2 && packed.sky[1] == 2.5f && packed.sky[2] == 4, "all sky lights contribute even beyond the direct budget");
    for (auto& actor : world.actors) actor.visible = false;
    const auto hidden = gatherLighting(world, 1000);
    require(hidden.info[1] == 1 && hidden.info[0] == 0 && hidden.info[3] == 0 && hidden.info[2] == -1,
        "hiding every light keeps authored mode without legacy sun or overflow");
    require(hidden.sky[0] == 0 && hidden.sky[1] == 0 && hidden.sky[2] == 0, "hiding sky removes environment contribution");
    world.actors.clear();
    const auto removed = gatherLighting(world, 3);
    require(removed.info[1] == 0 && removed.lights[0].colorIntensity[3] == 3, "removing all light actors restores legacy compatibility");
}
void legacyEnvironment() {
    World world;
    world.environment.areaLight.enabled = true;
    auto lighting = gatherLighting(world, 7);
    require(lighting.info[1] == 0 && lighting.info[0] == 1 && lighting.lights[0].colorIntensity[3] == 7,
        "legacy application lighting remains the default without scene override");
    require(lighting.sky[0] == 1 && lighting.info[2] == 0, "legacy sky and sun shadow remain available");
    world.environment.overrideLighting = true;
    world.environment.directionalIntensity = 4;
    world.environment.environmentIntensity = .25f;
    auto& rect = world.environment.areaLight;
    rect.center[0] = -3;
    rect.center[1] = 5;
    rect.halfAxisU[0] = 2;
    rect.halfAxisV[2] = 3;
    rect.radiance[0] = 13;
    rect.radiance[1] = 11;
    rect.radiance[2] = 7;
    lighting = gatherLighting(world, 99);
    require(lighting.info[1] == 1 && lighting.info[0] == 2 && lighting.lights[0].colorIntensity[3] == 4,
        "legacy scene override supplies sun and area light");
    require(lighting.sky[3] == 0, "legacy override keeps its existing PT emitter/MIS contract");
    require(lighting.sky[0] == .25f && lighting.sky[1] == .25f && lighting.sky[2] == .25f, "legacy environment scale reaches sky");
    const auto& area = lighting.lights[1];
    require(area.positionType[3] == 2 && area.positionType[0] == -3 && area.positionType[1] == 5, "legacy rectangle position and type survive");
    require(near(area.directionRange[1], -1) && area.directionRange[3] == 0, "legacy cross(U,V) emitting side and unlimited range survive");
    require(near(area.axisUWidth[3], 4) && near(area.axisVHeight[3], 6), "legacy half axes become full dimensions");
    require(area.colorIntensity[0] == 13 && area.colorIntensity[1] == 11 && area.colorIntensity[2] == 7 && area.colorIntensity[3] == 1,
        "legacy area radiance is not multiplied twice");
    world.actors.push_back(lightActor(LightType::Point));
    lighting = gatherLighting(world, 99);
    require(lighting.info[0] == 1 && lighting.lights[0].positionType[3] == 4 && lighting.sky[0] == 0,
        "authored lights take precedence over the complete legacy environment");
}
void shadowSunSelection() {
    World world;
    world.actors.push_back(lightActor(LightType::Point));
    auto sun = lightActor(LightType::Directional);
    sun.light->castShadows = false;
    world.actors.push_back(sun);
    sun.light->castShadows = true;
    sun.light->intensity = 0;
    world.actors.push_back(sun);
    sun.light->intensity = 5;
    sun.visible = false;
    world.actors.push_back(sun);
    sun.visible = true;
    world.actors.push_back(sun);
    world.actors.push_back(sun);
    require(gatherLighting(world, 5).info[2] == 3, "first visible nonzero shadow-casting sun wins after compacting hidden actors");
    world.actors[4].light->castShadows = false;
    require(gatherLighting(world, 5).info[2] == 4, "next shadow-casting sun takes over immediately");
    world.actors[5].visible = false;
    require(gatherLighting(world, 5).info[2] == -1, "no eligible sun leaves the map unassigned");
}
void parameterChanges() {
    World world;
    world.actors.push_back(lightActor(LightType::Spot));
    const auto baseline = gatherLighting(world, 5);
    const auto changed = [&](const auto& edit, const char* label) {
        auto copy = world;
        edit(copy.actors[0]);
        require(!(gatherLighting(copy, 5) == baseline), std::string(label) + " participates in light/GI change detection");
    };
    changed([](Actor& a) { a.light->type = LightType::Rect; }, "type");
    changed([](Actor& a) { a.light->color[0] = .2f; }, "red");
    changed([](Actor& a) { a.light->color[1] = .3f; }, "green");
    changed([](Actor& a) { a.light->color[2] = .4f; }, "blue");
    changed([](Actor& a) { a.light->intensity = 11; }, "intensity");
    changed([](Actor& a) { a.light->range = 12; }, "range");
    changed([](Actor& a) { a.light->innerConeAngle = 15; }, "inner cone");
    changed([](Actor& a) { a.light->outerConeAngle = 40; }, "outer cone");
    changed([](Actor& a) { a.light->width = 3; }, "width");
    changed([](Actor& a) { a.light->height = 4; }, "height");
    changed([](Actor& a) { a.light->castShadows = false; }, "cast shadows");
    changed([](Actor& a) { a.transform.position[2] = 2; }, "position");
    changed([](Actor& a) { a.transform.rotation[2] = 30; }, "rotation");
    changed([](Actor& a) { a.visible = false; }, "visibility");
    auto& light = *world.actors[0].light;
    light.innerConeAngle = light.outerConeAngle = 0;
    auto packed = gatherLighting(world, 5);
    require(packed.lights[0].spotShadow[0] == 1 && packed.lights[0].spotShadow[1] == 1, "valid zero-degree hard cone remains exact");
    light.innerConeAngle = light.outerConeAngle = 89.99f;
    packed = gatherLighting(world, 5);
    constexpr float radians = 3.14159265358979323846f / 180;
    require(near(packed.lights[0].spotShadow[1], std::cos(89.99f * radians), 1e-7f), "valid maximum cone is not narrowed to 89.9 degrees");
    require(packed.lights[0].spotShadow[0] == packed.lights[0].spotShadow[1], "equal authored angles preserve a hard edge");
    world.actors[0].transform.position[0] = std::numeric_limits<float>::quiet_NaN();
    packed = gatherLighting(world, 5);
    require(packed.info[0] == 0 && packed.info[1] == 1, "invalid runtime transform is skipped without restoring legacy lighting");
}
}

int main() {
    try {
        transformsAndTypes();
        budgetAndVisibility();
        legacyEnvironment();
        shadowSunSelection();
        parameterChanges();
        std::cout << "Lighting packing: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lighting packing failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
