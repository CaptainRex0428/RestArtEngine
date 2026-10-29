// GPU evidence for the camera effects, kept separate from the projection cases.
namespace {
void runCameraEffectsSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& directory, nlohmann::json& report) {
    using namespace rae;
    std::string error;
    assets::AssetLibrary assets; assets.createDefaults();
    for (unsigned i=0; i<2; ++i) {
        assets::MaterialAsset m; m.id=2001+i; m.name=i?"White bars":"Dark bars";
        m.grid=false; m.metallic=0; m.roughness=1;
        for (float& c:m.baseColor) c=i?.9f:.006f;
        assets.materials.push_back(m);
    }
    scene::World world; world.blackBackground=true;
    constexpr float tangent=.2679491924f; // tan(30 degree vertical FOV / 2).
    // Three equally sized projected stripe targets at 2, 5 and 9 metres.
    // Each has a known planar front face, so focus/blur can be tested by pixels.
    for (unsigned panel=0; panel<3; ++panel) {
        const float depth=panel==0?2.f:panel==1?5.f:9.f;
        const float unitsPerPixel=2*depth*tangent/height;
        for (unsigned stripe=0; stripe<12; ++stripe) {
            const float pixelX=25.f+panel*100.f+stripe*6.f+3.f;
            auto id=world.createActor("DOF stripe",3,2001+(stripe%2));
            auto& tr=world.findActor(id)->transform;
            tr.position[0]=(pixelX-width*.5f)*unitsPerPixel;
            tr.position[2]=-depth-.005f;
            tr.scale[0]=3*unitsPerPixel; tr.scale[1]=50*unitsPerPixel; tr.scale[2]=.005f;
        }
    }
    renderer::SceneSettings settings;
    settings.offscreenTarget=true; settings.viewportWidth=width; settings.viewportHeight=height;
    settings.cameraPose.emplace(); settings.cameraLens.verticalFovDegrees=30;
    settings.shadowMethod=renderer::ShadowMethod::Disabled;
    auto& dof=settings.cameraLens.depthOfField;
    dof.focusDistance=5; dof.fStop=.7f; dof.sensorHeightMm=36;
    const auto render=[&](const char* name) {
        diagnostics::beginFrame(); const bool ok=renderer.render(settings,world,assets,error); diagnostics::endFrame();
        require(ok,error); auto pixels=read(renderer);
        if(name) save(directory/(std::string(name)+".bmp"),pixels);
        return pixels;
    };
    const auto delta=[](const Pixels& a,const Pixels& b,int panel) {
        double total=0; unsigned n=0;
        for(unsigned y=100;y<140;++y) for(unsigned x=35u+unsigned(panel)*100u;x<87u+unsigned(panel)*100u;++x)
            for(unsigned c=0;c<3;++c) { total+=std::abs(int(a[(y*width+x)*4+c])-int(b[(y*width+x)*4+c])); ++n; }
        return total/n;
    };
    const auto contrast=[](const Pixels& pixels,int panel) {
        double total=0; unsigned n=0;
        for(unsigned y=100;y<140;++y) for(unsigned x=35u+unsigned(panel)*100u;x<81u+unsigned(panel)*100u;++x) {
            const double gradient=int(pixels[(y*width+x)*4])-int(pixels[(y*width+x+1)*4]);
            total+=gradient*gradient; ++n;
        }
        return std::sqrt(total/n);
    };
    require(renderer.ensureSceneTargetSize(width,height,error),error);
    const auto sharp=render("dof-disabled");
    dof.enabled=true;
    const auto blurred=render("dof-focus-middle");
    for(int panel=0;panel<3;++panel) {
        report["dofRaster"][std::to_string(panel)]={{"meanChange",delta(sharp,blurred,panel)},
            {"sharpContrast",contrast(sharp,panel)},{"blurredContrast",contrast(blurred,panel)}};
    }
    require(delta(sharp,blurred,1)<1.0,"DOF blurs the in-focus plane.");
    require(delta(sharp,blurred,0)>5 && delta(sharp,blurred,2)>5,"DOF does not blur both near and far targets.");
    require(contrast(blurred,0)<contrast(sharp,0)*.8 && contrast(blurred,2)<contrast(sharp,2)*.8,
        "DOF did not reduce out-of-focus high-frequency contrast.");
    dof.focusDistance=2;
    const auto focusNear=render("dof-focus-near");
    require(delta(sharp,focusNear,0)<1 && delta(sharp,focusNear,1)>5,"Focus distance does not move the sharp plane.");
    dof.focusDistance=5; dof.fStop=32;
    const auto closed=render("dof-closed-aperture");
    require(delta(sharp,closed,0)<delta(sharp,blurred,0)*.5,"Closing the aperture does not reduce blur.");
    dof.fStop=.7f; settings.path=renderer::RenderPath::Forward;
    const auto forward=render("dof-forward");
    require(delta(blurred,forward,0)<1 && delta(blurred,forward,2)<1,"Forward/Deferred DOF mismatch.");
    settings.path=renderer::RenderPath::Deferred;
    settings.debugView=2;
    const auto debugDof=render(nullptr); dof.enabled=false;
    require(debugDof==render(nullptr),"DOF modifies diagnostic data views.");
    settings.debugView=0; settings.cameraLens.projection=camera::Projection::Orthographic;
    const auto ortho=render(nullptr); dof.enabled=true;
    require(ortho==render(nullptr),"DOF must be inactive for orthographic projection.");
    settings.cameraLens.projection=camera::Projection::Perspective;
    dof.focusDistance=.01f;
    require(render(nullptr)==sharp,"Invalid focus plane must preserve pinhole rendering.");
    dof.focusDistance=5;
    // Candidate shader failures must preserve the active DOF pipeline too.
    require(!renderer.applyShaderSource("this is deliberately not HLSL",error),"Invalid shader unexpectedly compiled.");
    require(render(nullptr)==blurred,"Failed shader candidate destroyed the active DOF pipeline.");
    require(renderer.applyShaderSource("Surface EvaluateMaterial(float3 p,float3 n,Surface m) { return m; }",error),error);
    require(render(nullptr)==blurred,"Successful shader publication changed the DOF result unexpectedly.");
    std::cout<<"PASS: camera: DOF near/far blur, focus plane, aperture, Forward/Deferred, debug/ortho fallback and shader rollback.\n";
    report["checks"].push_back("DOF GPU focus, aperture, projection/debug fallbacks and candidate shader publication.");
    // All four independent outputs coexist; refreshing one cannot overwrite another.
    const renderer::PreviewSlot slots[]={renderer::PreviewSlot::Viewport0,renderer::PreviewSlot::Viewport1,
        renderer::PreviewSlot::Viewport2,renderer::PreviewSlot::Viewport3};
    Pixels views[4];
    for(unsigned i=0;i<4;++i) {
        auto view=settings; view.cameraPose->position[0]=float(i)*.3f;
        require(renderer.renderViewportPreview(view,world,assets,nullptr,200,150,error,slots[i]),error);
        unsigned w=0,h=0;
        require(renderer.readPreviewPixels(slots[i],views[i],w,h,error)&&w==200&&h==150,error);
        require(covered(views[i])>200,"Quad output is blank.");
    }
    for(unsigned i=0;i<4;++i) {
        Pixels now; unsigned w=0,h=0;
        require(renderer.readPreviewPixels(slots[i],now,w,h,error)&&now==views[i],"Another viewport overwrote a retained image.");
        if(i) require(views[i]!=views[0],"Independent viewport cameras produced an identical image.");
    }
    require(renderer.ensureSceneTargetSize(width,height,error),error);
    require(render(nullptr)==blurred,"Quad previews polluted main scene output.");
    std::cout<<"PASS: camera: Four independent viewport output textures and main image isolation.\n";
    report["checks"].push_back("Four independent viewport outputs preserve their own camera images.");
    if(renderer.hardwareRayTracingSupported()) {
        const auto start=renderer.hardwareRayTracingDispatchCount();
        settings.offscreenTarget=false; settings.editorViewport=true; settings.hardwareRayTracing=true;
        settings.globalIllumination.method=renderer::GlobalIlluminationMethod::PathTracing;
        settings.globalIllumination.mode=renderer::GlobalIlluminationMode::Progressive;
        settings.globalIllumination.samplesPerFrame=16; settings.globalIllumination.targetSamples=128;
        settings.globalIllumination.maxBounces=1;
        dof.enabled=false;
        for(unsigned i=0;i<8;++i) render(nullptr);
        const auto pinhole=render("dof-pt-pinhole");
        dof.enabled=true;
        render(nullptr);
        require(renderer.globalIlluminationStatus().accumulatedSamples==16,"Thin-lens enable did not reset progressive history.");
        for(unsigned i=1;i<8;++i) render(nullptr);
        const auto lens=render("dof-pt-thin-lens");
        report["dofPathTracing"]={{"nearChange",delta(pinhole,lens,0)}, {"focusChange",delta(pinhole,lens,1)},
            {"farChange",delta(pinhole,lens,2)}};
        require(delta(pinhole,lens,0)>5 && delta(pinhole,lens,2)>5,"Thin-lens rays do not blur near/far targets.");
        require(delta(pinhole,lens,1)<delta(pinhole,lens,0)*.5,"Thin-lens focal plane is not sharp.");
        dof.focusDistance=2; render(nullptr);
        require(renderer.globalIlluminationStatus().accumulatedSamples==16,"Thin-lens focus did not reset progressive history.");
        dof.fStop=4; render(nullptr);
        require(renderer.globalIlluminationStatus().accumulatedSamples==16,"Thin-lens aperture did not reset progressive history.");
        settings.viewId=27; render(nullptr);
        require(renderer.globalIlluminationStatus().accumulatedSamples==16,"Active viewport identity reused another view's history.");
        settings.globalIllumination.mode=renderer::GlobalIlluminationMode::Realtime;
        settings.globalIllumination.samplesPerFrame=1; dof.focusDistance=5; dof.fStop=.7f;
        render(nullptr); render(nullptr);
        const auto realtimeHistory=renderer.globalIlluminationStatus().historyRevision;
        dof.focusDistance=2; render("dof-realtime");
        require(renderer.globalIlluminationStatus().historyRevision==realtimeHistory,"Realtime postprocess DOF reset GI history.");
        const auto currentSamples=renderer.globalIlluminationStatus().accumulatedSamples;
        require(renderer.renderViewportPreview(settings,world,assets,nullptr,200,150,error,slots[2]),error);
        require(renderer.ensureSceneTargetSize(width,height,error),error); render(nullptr);
        require(renderer.globalIlluminationStatus().historyRevision==realtimeHistory &&
            renderer.globalIlluminationStatus().accumulatedSamples>=currentSamples,"Passive quad view invalidated active GI.");
        report["dofHardwareDispatches"]=renderer.hardwareRayTracingDispatchCount()-start;
        report["checks"].push_back("Real DXR thin-lens focus and progressive/realtime history semantics, quad isolation.");
        std::cout<<"PASS: camera: DXR thin-lens blur/focal plane, progressive lens and ViewId resets, realtime postprocess and quad history isolation.\n";
    }
}
}
