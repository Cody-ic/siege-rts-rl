# 精灵生成

两条路线，产物格式相同（`<标识符>_<朝向>.png`），前端代码不因换图而改动。

| 路线 | 脚本 | 用途 |
|---|---|---|
| **程序化占位** | `gen.py` | 第 1 周立刻可用，不依赖任何素材。尺寸与投影角必然一致 |
| **真实素材** | `blender_render.py` + `postprocess.py` | 正式画面。CC0 模型经统一相机与灯光预渲染成等距精灵 |

## 路线一：程序化占位

```bash
python gen.py out
```

输出 `out/<标识符>_<朝向>.png` 与 `out/_contact_sheet.png`。无依赖（仅 Pillow），
用于在美术就位前解锁前端开发。

## 路线二：真实素材

### 1. 只用 CC0 素材

**这是硬性要求，不是偏好。** 交付清单要求提交「软件源代码，包括输入文件」，
素材需随仓库提交。CC0 允许无条件复制、修改、再分发；其他"免费"许可通常不允许。

已核实可用的来源：

| 来源 | 许可 | 覆盖 |
|---|---|---|
| [Quaternius](https://quaternius.com/) | CC0 | knight、monsters、Animated Animal Pack（含 **Horse**）、Fantasy Props |
| [KayKit](https://kaylousberg.itch.io/) | CC0 | **Skeletons**、Adventurers、**Medieval Builder**、Dungeon |
| [Kenney](https://kenney.nl) | CC0 | 地形、瓦片、建筑 |
| [Poly Pizza](https://poly.pizza/) | **混杂** | 聚合站，含大量 CC-BY 的 Google Poly 存档，**须逐个查看许可标签** |

**不要使用 [Reiner's Tilesets](https://www.reinerstilesets.de/graphics/lizenz/)**：
其许可禁止将原始素材（含修改版）上传到自己的页面，与本项目"素材入库并随源码交付"冲突。

### 素材包本身的坑（下次换素材大概率还会撞上）

| 症状 | 原因 | 应对 |
|---|---|---|
| 模型整体渲成**品红** | `.blend` 把纹理路径写成与自身同级，分发时纹理却在单独的 `Textures/` 目录 | `relocate_missing_textures()` 按文件名逐级向上搜索并改写 `filepath`。Blender 的 `file.find_missing_files` 算子在无头模式下不可靠 |
| 渲出**空图** | 网格被作者设了 `hide_render=True`（Quaternius 的 Skeleton 即如此） | `normalize()` 中强制 `hide_render = hide_viewport = False` |
| 模型停在**默认姿态**（鹰只剩两片下垂的翅膀） | `libraries.load` 会载入动作数据块，但不会绑到骨架 | `set_action()` 显式指定；动作名各包不同（`Idle` / `Skeleton_Idle` / `Bat_Flying`），找不到时会打印可用列表 |
| 跨包组合比例失调 | 各包尺度互不相干（马高 6.91、骷髅高 5.09，原始单位无可比性） | `parts` 的 `scale` 先对齐比例，再用 `offset` 定位 |

另外两个已知坑：Quaternius 的 **FBX 动画会损坏**，请用 `.blend` 或 glTF；
KayKit 的 **Legacy 版本**风格陈旧，须取重做后的新版。

### 2. 放置模型并登记

模型放在 `models/<素材包>/`（**保留素材包原目录结构**，否则 `.blend` 引用的纹理会失效），
然后在 `assets.json` 中登记。各家模型的朝向、尺度、原点、动作名都不一致，
这些差异**全部在 JSON 里用数据抹平**，不改脚本。

登记项：`cn` 中文名、`yaw` 朝向校正、`height` / `max_width` 尺寸、`lift` 抬升（空中单位）、
`action` / `frames` 动作与帧、`tint` 阵营染色。

**模块化素材用 `parts` 拼装**：Kenney 城堡包的塔身分为底座/中段/顶层/屋顶数块，
每块都从 z=0 起建模，单独渲染只是塔的一截。`parts` 按累计高度自下而上堆叠，
`gap` 可微调各段间距（旗帜等装饰件常需负值下沉）。

### 3. 渲染

```bash
blender --background --python blender_render.py -- --manifest assets.json --out out_3d
blender --background --python blender_render.py -- --only Knight,Phoenix   # 只渲部分
python postprocess.py out_3d
```

## 为什么混用不同来源不会翻车

混用 **2D** 等距精灵几乎必然翻车——投影角、比例、光照方向已经烤死在像素里，无法统一。

混用 **3D** 模型则可以救：所有模型都经过**同一个正交相机 + 同一套三点灯光 + 同一分辨率**，
再统一缩放到规定高度。风格差异在渲染阶段就被压平了。这正是选择预渲染路线的原因。

## 关键约定

- **相机俯角 30°**（绕 X 60°、绕 Z 45°），投影瓦片为 **2:1**，与 `isolib.TILE_W/TILE_H` 一致。改这个值等于改掉整套素材。
- **旋转模型而非旋转相机。** 相机与灯光固定，各朝向受光不同，符合等距游戏观感；旋转相机会让模型受光恒定，画面发平。
- **描边与地面投影在后处理做，不在 Blender 做。** 描边保证暗色单位在暗色地表上剪影可读；地面投影同时承担"表达高度"的职责——空中单位投影留在地面、本体抬高，玩家一眼看出它在飞。

## 前端契约：`_sprite_meta.json`

随精灵输出，**这是前端唯一应当依赖的尺寸约定**：

```json
{
  "px_per_tile": 128,
  "sprites": {
    "Spear": { "cn": "铁壁枪卫", "canvas": [242, 172], "ground_anchor": [121.0, 143.8] },
    "Keep":  { "cn": "领主堡垒", "canvas": [312, 332], "ground_anchor": [156.0, 303.8] }
  }
}
```

**画布尺寸逐个实体适配，因此 `canvas` 与 `ground_anchor` 必须按精灵读取。**

实体高度跨度约 5 倍（幽影窥使 0.45 瓦片 ~ 领主堡垒 2.15 瓦片）。统一画布只能二选一：
要么裁掉高的，要么把矮的挤成一小点。因此本流水线**固定世界→像素缩放**（`px_per_tile`），
只让画布随实体变化——相对大小因此始终正确，单位本就应当显得比塔矮。

`ground_anchor` 是画布内**代表实体脚底的像素坐标**，前端据此把精灵对齐到格子中心。

- 由**相机投影精确算出**（世界原点的投影位置），不是估算
- **不要靠 alpha 包围盒底边推算**——实体带披风、长矛、尾羽、旗帜时会明显偏移
- 相机 `shift_y` 由 `frame_for()` 按实体高度求出，使地面点落在距底边 `margin` 处

`--only` 部分重渲时元数据会与已有条目**合并**，不会丢掉其余精灵。

## 后处理是幂等的

`postprocess.py` 默认原地覆盖输入目录。为避免重复运行不断叠加描边与投影（且**不报错**，只是一次比一次糟），处理过的 PNG 会写入文本标记 `siege_postprocessed`，再次运行时自动跳过。

`--force` 可强制重处理，但**只应对未经后处理的原始渲染输出使用**。

## 精灵成品入库

`out_3d/` 的 77 张精灵与 `_sprite_meta.json` **入库**，理由三条：

1. 交付清单要求「软件源代码**包括输入文件和输出文件**」——精灵既是渲染的输出、也是游戏的输入
2. 体积可忽略：共约 2.7 MB，远小于已入库的 `.glb` 源素材
3. "产物可重建所以不入库"在此不成立——重建需装 Blender 并配好环境，成本不低，前端不应为此被阻塞

`out/`（占位）与 `dl/`（下载的原始压缩包）仍然忽略，前者可由 `gen.py` 秒级重建，后者已解压进 `models/`。
