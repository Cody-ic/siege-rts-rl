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

两个已知坑：Quaternius 的 **FBX 动画会损坏**，请用 `.blend` 或 glTF；
KayKit 的 **Legacy 版本**风格陈旧，须取重做后的新版。

### 2. 放置模型并登记

模型放在 `models/<阵营>/<名称>.glb`，然后在 `assets.json` 中登记。
各家模型的朝向、尺度、原点都不一致，这些差异**全部在 JSON 里用数据抹平**，不改脚本。

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

两条路线都会随精灵输出一份元数据，**这是前端唯一应当依赖的尺寸约定**：

```json
{ "canvas": 128, "tile": [64, 32], "ground_anchor": [64.0, 92.2], "dirs": ["SE","SW","NE","NW"] }
```

`ground_anchor` 是画布内**代表单位脚底的像素坐标**，前端据此把精灵对齐到格子中心。

- 3D 路线由**相机投影精确算出**（世界原点投影位置），不是估算
- 占位路线取自 `isolib.ANCHOR`，两者已对齐（92.2 vs 92，差 0.2 像素）
- **不要靠 alpha 包围盒底边推算**——单位带披风、长矛、尾羽时会明显偏移

相机的 `shift_y = 0.22` 使地面点落在画布约 72% 高度处而非正中；不做偏移的话下半张全是空白，而向上延伸的长矛与塔顶反而会顶出画面。

## 后处理是幂等的

`postprocess.py` 默认原地覆盖输入目录。为避免重复运行不断叠加描边与投影（且**不报错**，只是一次比一次糟），处理过的 PNG 会写入文本标记 `siege_postprocessed`，再次运行时自动跳过。

`--force` 可强制重处理，但**只应对未经后处理的原始渲染输出使用**。

## 交付提醒

`.gitignore` 忽略 `out/` 与 `out_3d/`（可由脚本重建），但交付清单要求「软件源代码**包括输入文件和输出文件**」——**最终打包时精灵 PNG 必须在包里**，不要因 gitignore 而漏掉。
