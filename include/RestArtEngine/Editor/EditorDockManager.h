#pragma once

#include <array>
#include <functional>

namespace rae::editor {

enum class EditorPanel { Outliner, Scene, Details, Settings, Content, Viewport, Count };

// Owns panel identity and default placement. ImGui owns the live split/tab tree
// and serializes it together with floating window geometry in its ini file.
class EditorDockManager {
public:
    static const char* panelName(EditorPanel panel);
    void beginFrame(float toolbarHeight);
    // Preserve the dock tree while only an independent tool window is drawn.
    void keepAlive();
    void drawPanel(EditorPanel panel, const std::function<void()>& contents);
    void drawLayoutMenu();
    // Event-driven activation; consumed once by the next workspace frame.
    void requestFocus(EditorPanel panel) { focusRequested_ = panel; }
    void resetLayout() { resetRequested_ = true; }
private:
    enum class Slot { LeftTop, LeftBottom, Right, Bottom, Center };
    struct Panel { const char* name; Slot slot; };
    static const std::array<Panel, static_cast<unsigned>(EditorPanel::Count)> panels_;
    void buildDefault(unsigned dockId, float width, float height);
    void migrateScenePanel();
    void applyFocusRequest();
    bool resetRequested_ = false;
    EditorPanel focusRequested_ = EditorPanel::Count;
};

} // namespace rae::editor
