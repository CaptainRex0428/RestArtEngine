# 图标资源（assets/icons）

编辑器与工具窗口使用的界面图标。所有图标都是线框风格、透明背景，颜色统一为 `#E6E6E6`，运行时由 UI 代码按主题色着色，不要在图里烘焙彩色。

## 目录与命名

```
assets/icons/
  <area>/                     功能区域，例如 outliner-eye
    <name>_<state>.svg        矢量源文件（首选，可无损缩放）
    <name>_<state>_32.png     32px 位图导出
    <name>_<state>_64.png     64px 位图导出（@2x）
    variants/                 方案探索与备选稿，不是线上资源
```

- `<area>`：图标所属面板或功能，例如 `outliner-eye`、`toolbar`。
- `<name>`：图标语义名，用短横线分词，例如 `eye`、`lock`。
- `<state>`：状态后缀，例如 `visible` / `hidden`、`normal` / `disabled`。
- 尺寸只在位图导出时出现；矢量源文件不带尺寸后缀。

## 绘制规范

| 项 | 约定 |
|---|---|
| 画布 | 24×24 viewBox，四周留 2px 视觉内边距 |
| 线条 | 圆头圆角端点，默认线宽 1.7（细线变体 1.3） |
| 颜色 | 描边 `#E6E6E6`，透明背景；不填彩色 |
| 状态 | 同一图标必须提供可见 / 隐藏两个语义状态，形状本身可区分 |
| 最小尺寸 | 16px 下仍能辨认；小于 16px 的图标另行简化 |

## 当前资源

- `outliner-eye/eye_visible.*`、`outliner-eye/eye_hidden.*`：World Outliner 每行的可见性小眼睛，方案 B（空心瞳孔 + 隐藏斜线）。隐藏状态的描边不透明度降到约 60%，另外运行时还会配合 `TextDisabled` 名称颜色。
- `outliner-eye/variants/`：A–F 六版方案与对比图 `contact_sheet.png`，供后续改版参考。

## 代码中的使用

目前 Outliner 小眼睛由 `src/Editor/SceneEditor.cpp` 的 `drawEyeIcon` 用 `ImDrawList` 直接绘制，因此任意 DPI 都清晰，也不依赖运行时图片解码。这里的 SVG/PNG 是这套图标的**设计源**：需要换成贴图实现时，直接加载同目录的 PNG 即可，几何与颜色以本目录为准。