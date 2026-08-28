# render — 正式前端

**不是调试视图。** `CLAUDE.md` 明写「`render/` 不是『调试视图』，而是正式前端，
工作量要按正式前端估，**预算 5–8 人日**，不要按『画几个方块』来排期」。
它同时兼任 RL 调试工具——调策略时必须能看观测张量、奖励信号与目标选择。

> 设计决策不在本文。契约在 `CLAUDE.md` 与 `地图与场景设计.md`，
> 本文只写「怎么跑」与「分了几期」。

## 怎么跑

`render/` **默认不参与构建**（不变量 2、3 的构建侧落地），要显式打开：

```bash
cmake -B build -DRTS_BUILD_RENDER=ON
cmake --build build --config Release

# 窗口模式
./build/render/Release/rts_render.exe \
    --map game/testdata/fixture_min.json \
    --sprites tools/sprite_gen/out_3d

# 截图模式：渲一帧导出成 PNG 后退出，不开窗口
./build/render/Release/rts_render.exe \
    --map game/testdata/fixture_min.json \
    --sprites tools/sprite_gen/out_3d \
    --screenshot out.png --size 1200 700
```

窗口模式下：方向键 / WASD 平移，滚轮缩放（以鼠标为锚），中键拖拽，`F` 重新入画。

**首次配置要下 42 MB 的 raylib 源码包。** 已有本地副本时可以完全不走网络：

```bash
cmake -B build -DRTS_BUILD_RENDER=ON -DFETCHCONTENT_SOURCE_DIR_RAYLIB=<路径>
```

### 截图模式为什么存在

不是为了省事，是为了**让渲染结果不必靠人盯着看才能验**：

- 评审能跑一遍、对着 PNG 看，不必自己开窗
- 有回归测试可挂（`ctest -R render_smoke`）
- `#16` 的「双视图」（真实战场 vs AI 以为的战场）本来就要离屏渲染，是同一套手法

它用 `RenderTexture` 而不是 `TakeScreenshot`：**隐藏窗口的默认帧缓冲在 Windows 上
读回来是黑的**（实测），而离屏纹理不依赖窗口可见性。

## 分层

```
SpriteAtlas      读 _sprite_meta.json + 按需载入 PNG，给出 (纹理, 地面锚点)
IsoProjection    格坐标 -> 屏幕像素。**唯一知道 px_per_tile 的地方**
SceneRenderer    两遍绘制：先整片铺地砖，再按深度键画叠加物与实体
CameraController 平移 / 缩放 / 让整张地图入画
```

**那些该被测的规则不在这里**，在 `game/`：地形展开、变体挑选、走向推导、深度排序。
理由是 `render/` 默认不配置，挂在它下面的测试在默认构建里根本不存在、ctest 照样全绿，
而那几条正是「只值一行代码但错了才发现」的那种。见 `game/CMakeLists.txt` 开头。

于是 `render/` 只剩「拿绘制列表去 blit」，而它拿到的绘制列表里**没有可变的仿真状态**
——不变量 2 因此是类型层面的保证，不是约定。

## 已知限制与分期

### 现在能做什么

读一张地图文件、画出地形与初始墙段、平移缩放。**没有实体渲染**（等 `rts_core` 接口）。

### 屏幕文字暂时只能是 ASCII

raylib 的内置字体没有 CJK 字形，中文会渲成一串 `?`（实测）。要中文得随包分发一个
字体文件并指定码点集合——那与 ImGui 的字体是同一件事，一起做。
在此之前屏幕提示刻意写英文，而不是渲一行问号出来。

**注意这条与「命名纪律」不冲突**：代码标识符一律英文、中文只出现在展示层，
而这里正是展示层暂时做不到中文。控制台输出（`--map` 的地图名等）没有这个问题。

### 后续

| 期 | 内容 | 前置 |
|---|---|---|
| ~~PR A~~ | `game/` 地图加载 + 场景装配 | — |
| ~~PR B~~ | 本目录：世界渲染 + 相机 + 截图模式 | — |
| PR C | Dear ImGui + rlImGui、HUD 骨架、CJK 字体、RL 调试面板 | — |
| PR D | 实体渲染、血条、选择框、建造虚影 | `rts_core` 接口定稿 |

PR D 只需要加一个「只读视图 → 绘制列表」的适配函数，本目录的代码不用改——
`DrawItem` 从一开始就与 `rts_core` 解耦。
