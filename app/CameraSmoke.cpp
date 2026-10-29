#include "CameraSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Pixels = std::vector<std::uint8_t>;
constexpr unsigned width = 320, height = 240;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error("Camera: " + message); }
Pixels read(rae::renderer::SceneRenderer& renderer) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "Missing scene output.");
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture, staging;
    require(SUCCEEDED(resource.As(&texture)), "Scene output is not a texture.");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    require(desc.Width == width && desc.Height == height && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Wrong image size/format.");
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.BindFlags = desc.MiscFlags = 0;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)), "Staging allocation failed.");
    renderer.context()->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Readback failed.");
    Pixels result(width * height * 4);
    for (unsigned y=0; y<height; ++y) std::memcpy(result.data()+y*width*4,
        static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch, width*4);
    renderer.context()->Unmap(staging.Get(), 0); return result;
}
void save(const std::filesystem::path& path, Pixels pixels) {
    for (std::size_t i=0; i<pixels.size(); i+=4) std::swap(pixels[i],pixels[i+2]);
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(info); file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    info.biSize=sizeof(info); info.biWidth=width; info.biHeight=-static_cast<LONG>(height);
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(&file),sizeof(file));
    out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    require(bool(out),"Cannot write evidence.");
}
std::size_t covered(const Pixels& image) {
    std::size_t result=0;
    for(std::size_t i=0;i<image.size();i+=4) if(image[i]>5 || image[i+1]>5 || image[i+2]>5) ++result;
    return result;
}
}
#include "CameraEffectsSmoke.inl"
void runCameraSmoke(rae::renderer::SceneRenderer& renderer, const std::filesystem::path& directory) {
    using namespace rae;
    std::filesystem::create_directories(directory);
    nlohmann::json report{{"passed",false},{"adapter",renderer.adapterName()},{"checks",nlohmann::json::array()}};
    const auto persist=[&] { std::ofstream out(directory/"camera-report.json"); out<<report.dump(2)<<'\n'; };
    const auto pass=[&](const char* message) { report["checks"].push_back(message); std::cout<<"PASS: camera: "<<message<<'\n'; };
    try {
        std::string error;
        require(renderer.ensureSceneTargetSize(width,height,error),error);
        require(renderer.applyShaderSource("Surface EvaluateMaterial(float3 p,float3 n,Surface m) { return m; }",error),error);
        assets::AssetLibrary assets; assets.createDefaults();
        assets::MaterialAsset material; material.id=1001; material.name="Camera red"; material.grid=false;
        material.baseColor[0]=.7f; material.baseColor[1]=.12f; material.baseColor[2]=.04f; material.metallic=0; material.roughness=.5f;
        assets.materials.push_back(material);
        scene::World world; world.blackBackground=true;
        world.createActor("Camera fixture cube",3,material.id);
        renderer::SceneSettings settings;
        settings.offscreenTarget=true; settings.viewportWidth=width; settings.viewportHeight=height;
        settings.shadowMethod=renderer::ShadowMethod::Disabled; settings.debugView=1;
        settings.cameraPose.emplace(); settings.cameraPose->position[2]=5;
        const auto render=[&](const char* name) {
            diagnostics::beginFrame(); const bool ok=renderer.render(settings,world,assets,error); diagnostics::endFrame();
            require(ok,error); auto pixels=read(renderer); if(name) save(directory/(std::string(name)+".bmp"),pixels); return pixels;
        };
        const auto perspectiveNear=render("perspective-near");
        settings.cameraPose->position[2]=10;
        const auto perspectiveFar=render("perspective-far");
        require(covered(perspectiveNear)>covered(perspectiveFar)*2,"Perspective distance did not change apparent size.");
        settings.cameraLens.projection=camera::Projection::Orthographic; settings.cameraLens.orthographicHeight=4;
        settings.cameraPose->position[2]=5;
        const auto orthoNear=render("orthographic-near");
        settings.cameraPose->position[2]=10;
        const auto orthoFar=render("orthographic-far");
        require(covered(orthoNear)>100 && orthoNear==orthoFar,"Orthographic image changes scale with distance.");
        pass("Perspective scales with distance; orthographic image remains pixel-identical.");
        settings.cameraPose->position[2]=5;
        settings.cameraLens.farClip=3;
        require(covered(render("far-clipped"))==0,"Far clipping did not reject the cube.");
        settings.cameraLens.farClip=100; settings.cameraLens.nearClip=8;
        require(covered(render("near-clipped"))==0,"Near clipping did not reject the cube.");
        settings.cameraLens.nearClip=.05f;
        settings.debugView=0; settings.path=renderer::RenderPath::Forward;
        const auto forward=render("orthographic-forward");
        settings.path=renderer::RenderPath::Deferred;
        const auto deferred=render("orthographic-deferred");
        double difference=0; for(std::size_t i=0;i<forward.size();i+=4) for(unsigned c=0;c<3;++c)
            difference+=std::abs(int(forward[i+c])-int(deferred[i+c]));
        difference/=width*height*3;
        report["forwardDeferredMeanError"]=difference;
        require(covered(forward)>100 && difference<1,"Orthographic Forward/Deferred shading differs.");
        pass("Near/far clipping and orthographic Forward/Deferred shading agree.");
        for(auto shadow : {renderer::ShadowMethod::ShadowMapCSM2,renderer::ShadowMethod::AnalyticRay}) {
            settings.shadowMethod=shadow; require(covered(render(nullptr))>100,"Orthographic shadow path lost visible geometry.");
        }
        settings.shadowMethod=renderer::ShadowMethod::Disabled;
        settings.debugView=8;
        const auto depthNear=render("orthographic-depth-near"); settings.cameraPose->position[2]=15;
        const auto depthFar=render("orthographic-depth-far");
        require(depthNear!=depthFar,"Orthographic linear depth does not respond to distance.");
        settings.cameraPose->position[2]=5; settings.debugView=0;
        pass("Orthographic CSM/analytic paths render; linear depth responds to distance.");
        // Preview is an independent image; it must not change the requested camera.
        const auto camera=renderer::resolveCamera(settings);
        require(renderer.renderCameraPreview(world,assets,camera,200,112,error),error);
        std::vector<std::uint8_t> preview; unsigned pw=0,ph=0;
        require(renderer.readPreviewPixels(renderer::PreviewSlot::Camera,preview,pw,ph,error)&&pw==200&&ph==112,error);
        require(covered(preview)>30,"Camera preview has no visible geometry.");
        require(renderer.ensureSceneTargetSize(width,height,error),error);
        const auto afterPreview=render(nullptr);
        require(afterPreview==deferred,"Camera preview polluted main render state.");
        pass("Independent camera preview renders and preserves the main view.");
        world.blackBackground=false;
        settings.viewportWidth=settings.viewportHeight=0; settings.cameraLens.aspectRatio=1;
        const auto fitted=render("runtime-square-frame");
        settings.viewportX=40; settings.viewportY=0; settings.viewportWidth=240; settings.viewportHeight=240;
        const auto explicitFrame=render(nullptr);
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) for(unsigned c=0;c<3;++c) {
            const auto p=(y*width+x)*4+c;
            require(x<40 || x>=280 ? fitted[p]==0 : fitted[p]==explicitFrame[p],
                "Runtime fixed aspect stretches/crops the image or leaves nonblack bars.");
        }
        settings.viewportX=settings.viewportY=settings.viewportWidth=settings.viewportHeight=0;
        settings.cameraLens.aspectRatio=2;
        const auto wide=render("runtime-wide-frame");
        for(unsigned y=0;y<height;++y) if(y<40 || y>=200)
            for(unsigned x=0;x<width;++x) for(unsigned c=0;c<3;++c)
                require(wide[(y*width+x)*4+c]==0,"Runtime fixed aspect has nonblack top/bottom bars.");
        pass("Runtime automatic fixed-aspect framing agrees with an explicit viewport and clears black bars.");
        world.blackBackground=true; settings.cameraLens.aspectRatio=0;
        settings.viewportWidth=width; settings.viewportHeight=height;
        settings.offscreenTarget=false; settings.editorViewport=true; settings.hardwareRayTracing=true;
        settings.globalIllumination.method=renderer::GlobalIlluminationMethod::PathTracing;
        settings.globalIllumination.samplesPerFrame=1; settings.globalIllumination.targetSamples=16;
        const auto startDispatch=renderer.hardwareRayTracingDispatchCount();
        render(nullptr);
        if(renderer.hardwareRayTracingSupported()) {
            require(renderer.hardwareRayTracingActive(),renderer.hardwareRayTracingStatus());
            render(nullptr);
            const auto first=renderer.globalIlluminationStatus();
            require(first.accumulatedSamples==2,"Orthographic GI does not accumulate.");
            require(renderer.renderCameraPreview(world,assets,camera,200,112,error),error);
            require(renderer.ensureSceneTargetSize(width,height,error),error);
            render(nullptr);
            require(renderer.globalIlluminationStatus().accumulatedSamples==3,"Preview resets the main GI history.");
            settings.exposure=1.2f; render(nullptr);
            require(renderer.globalIlluminationStatus().accumulatedSamples==4,"Exposure resets linear GI history.");
            settings.cameraLens.orthographicHeight=5; render(nullptr);
            require(renderer.globalIlluminationStatus().accumulatedSamples==1,"Lens change does not reset GI.");
            settings.cameraLens.projection=camera::Projection::Perspective; render(nullptr);
            require(renderer.globalIlluminationStatus().accumulatedSamples==1,"Projection change does not reset GI.");
            settings.cameraLens.projection=camera::Projection::Orthographic;
            settings.globalIllumination.mode=renderer::GlobalIlluminationMode::Realtime;
            const auto realtime=render("orthographic-realtime-gi");
            require(covered(realtime)>100,"Orthographic realtime GI has no visible geometry.");
            report["hardwareDispatches"]=renderer.hardwareRayTracingDispatchCount()-startDispatch;
            pass("Real DXR orthographic GI, lens invalidation, exposure and preview history isolation.");
        } else { report["hardwareSkip"]=renderer.hardwareRayTracingStatus(); std::cout<<"SKIP: camera DXR: "<<renderer.hardwareRayTracingStatus()<<'\n'; }
        runCameraEffectsSmoke(renderer, directory, report);
        require(renderer.validationErrorCount()==0,"Graphics validation errors.");
        report["validationErrors"]=renderer.validationErrorCount(); report["passed"]=true; persist();
    } catch(const std::exception& error) { report["failure"]=error.what(); persist(); throw; }
}
