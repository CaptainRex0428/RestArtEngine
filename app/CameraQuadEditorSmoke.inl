// Runs in the isolated Camera fixture, using the same real ImGui input hook.
{
    const auto beforeQuad=settings;
    settings.cameraLens.projection=camera::Projection::Perspective;
    settings.cameraPose.reset(); settings.cameraYaw=.65f; settings.cameraPitch=.3f;
    settings.cameraLens.aspectRatio=0;
    editor.setQuadView(true);
    capture("camera_quad_warmup",true);
    capture("camera_quad_ready",true,true);
    require(editor.quadView() && !editor.viewportMaximized(),"Four-view layout did not open.");
    std::array<std::array<float,4>,4> rects;
    for(int i=0;i<4;++i) {
        rects[i]=editor.viewportImageRect(i);
        require(rects[i][2]-rects[i][0]>100 && rects[i][3]-rects[i][1]>70,"A quad viewport has no usable image.");
        for(int j=0;j<i;++j)
            require(rects[i][0]>=rects[j][2]-1 || rects[j][0]>=rects[i][2]-1 ||
                rects[i][1]>=rects[j][3]-1 || rects[j][1]>=rects[i][3]-1,"Quad image rectangles overlap.");
    }
    std::array<camera::Camera,4> beforeNavigation;
    for(int i=0;i<4;++i) {
        editor.setActiveViewport(i); capture("camera_quad_activate",true);
        require(editor.activeViewportIndex()==i,"Could not activate requested viewport.");
        beforeNavigation[i]=renderer::resolveCamera(settings);
        require(settings.cameraLens.projection==(i?camera::Projection::Orthographic:camera::Projection::Perspective),
            "Default quad projection does not match Perspective/Top/Front/Right.");
    }
    editor.setActiveViewport(0); capture("camera_quad_before_click",true);
    // Click a different image through ordinary input, then pan only that view.
    auto rect=editor.viewportImageRect(2);
    input.mouse={rect[0]+(rect[2]-rect[0])*.8f,rect[1]+(rect[3]-rect[1])*.75f};
    click(input.mouse); capture("camera_quad_clicked",true);
    require(editor.activeViewportIndex()==2,"Clicking another view did not route active input.");
    const auto quadHistory=editor.undoStack().cursor();
    input.events=[](ImGuiIO& io){io.AddMouseButtonEvent(2,true);};
    capture("camera_quad_pan_down",true);
    for(int step=0;step<3;++step){input.mouse.x+=12;capture("camera_quad_pan_move",true);}
    input.events=[](ImGuiIO& io){io.AddMouseButtonEvent(2,false);};
    capture("camera_quad_pan_up",true);
    const auto navigated=renderer::resolveCamera(settings);
    require(std::memcmp(&navigated.pose,&beforeNavigation[2].pose,sizeof(camera::Pose))!=0,
        "Quad middle-mouse navigation did not move the active camera.");
    require(editor.undoStack().cursor()==quadHistory,"Free quad navigation created an Actor undo transaction.");
    for(int i : {0,1,3}) {
        editor.setActiveViewport(i); capture("camera_quad_isolation",true);
        const auto current=renderer::resolveCamera(settings);
        require(std::memcmp(&current.pose,&beforeNavigation[i].pose,sizeof(camera::Pose))==0,
            "Navigating one quad view moved another camera.");
    }
    editor.setActiveViewport(2); capture("camera_quad_restore_active",true);
    const auto restoredNavigation=renderer::resolveCamera(settings);
    require(std::memcmp(&restoredNavigation.pose,&navigated.pose,sizeof(camera::Pose))==0,
        "Switching views lost the navigated camera state.");
    editor.setViewportMaximized(true); capture("camera_quad_maximized",true,true);
    const auto maxRect=editor.cameraImageRect();
    require(editor.viewportMaximized() && maxRect[2]-maxRect[0]>(rect[2]-rect[0])*1.5f,
        "Maximize did not enlarge the active view.");
    editor.setViewportMaximized(false); capture("camera_quad_restored",true);
    require(!editor.viewportMaximized(),"Quad restore did not restore the layout.");
    for(int i : {1,2,3}) {
        editor.setActiveViewport(i); capture("camera_quad_pick_prepare",true);
        editor.executeConsoleCommand("select " + std::to_string(id));
        capture("camera_quad_pick_camera",true);
        const auto view=camera::evaluate(renderer::resolveCamera(settings),float(settings.viewportWidth)/settings.viewportHeight);
        // Lower-right part of the cube, clear of the toolbar and marker handles.
        const float x=.45f*(view.right[0]-view.up[0]);
        const float y=1+.45f*(view.right[1]-view.up[1]);
        const float z=.45f*(view.right[2]-view.up[2]);
        click(projectPoint(x,y,z));
        require(editor.selectedActor()==subject,"An orthographic quad view picked the wrong Actor.");
    }
    for(const auto theme : {imgui::EditorTheme::Black,imgui::EditorTheme::DarkBlue}) {
        imgui::setTheme(theme); capture("camera_quad_theme_warmup",true);
        capture(theme==imgui::EditorTheme::Black?"camera_quad_black":"camera_quad_blue",true,true);
    }
    editor.setQuadView(false); capture("camera_quad_single",true);
    require(!editor.quadView(),"Single viewport mode was not restored.");
    settings=beforeQuad;
    std::cout<<"PASS: Quad independent image rectangles, default projections, real click/pan routing, isolated camera state, orthographic picking, maximize/restore and both themes.\n";
}
