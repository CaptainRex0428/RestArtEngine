# Shadow methods

`SceneSettings::shadowMethod` controls the direct-light shadow strategy in the
DX11 preview. The same selection is used by Forward and Deferred rendering;
screen-space methods use the Deferred G-buffer and fall back to the raster map
when Forward is selected.

| Method | Resource path | Best use | Tradeoff |
| --- | --- | --- | --- |
| 2x2 PCF | One 2048² depth map | Stable default preview | Fixed directional-light resolution |
| Hard | One depth map, one comparison | Debugging shadow edges | Aliased edges |
| 3x3 PCF | One depth map, nine comparisons | Softer raster shadow edges | More samples |
| PCSS | One depth map, blocker search + PCF | Variable soft penumbra | Highest raster sampling cost |
| 2-cascade CSM | Two-slice depth texture array | Better near/far directional shadow coverage | Two shadow passes and a cascade transition |
| GTAO | Deferred position/normal buffers | Ambient contact darkening | Screen-space only; view-dependent |
| Deferred Contact Shadow | Deferred position buffer + short light march | Tight contact under objects | Screen-space only; Forward falls back to depth shadow |
| Analytic rays | Built-in intersections + triangle buffer | Mesh-accurate contact and reflection rays | Linear triangle traversal; no hardware DXR |
| Disabled | No direct-light shadow term | Lighting/debug comparison | No direct shadows |

The depth-array and shadow constants are owned by `SceneRenderer`; shader
sampling and mode selection live in `Shaders/Private/Scene.hlsl`; the editor
only changes `SceneSettings`. This keeps project assets and editable material
shaders independent from renderer shadow resources.

GPU validation coverage is part of `EditorSmoke`: it renders PCSS, CSM2, GTAO,
Deferred Contact Shadow, AnalyticRay, Disabled, and the default PCF mode, and
checks the CSM/PCSS/GTAO/contact/analytic outputs against the disabled-shadow capture.
