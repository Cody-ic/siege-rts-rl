#!/usr/bin/env python3
"""校验 `assets.json`：模型路径存在、且不同实体不共用主体模型。

**为什么要这条检查。** 这个坑已经踩过两次，两次都是渲完看图才发现：

1. 采石场与岩壁同为 `rocks-large`，而一个是必须守的经济建筑、一个是走不进去的地形
2. 伐木场与木栅同为 `wall-narrow-wood-fence`，而木栅是波次中可即时放置的应急工事

两次的形态一样：**两个机制完全不同的实体共用一张图**。而 CLAUDE.md 的
「风味名不得遮蔽机制可读性」要求玩家能一眼读懂画面——共用主体模型直接违反它。

登记新实体时靠人记得"这个模型有没有被别人用过"是不现实的：`models/` 下有一百多个
文件、`assets.json` 有三十多个实体。所以把它变成一条会失败的检查，
与 `tools/check_determinism_bans.py` 是同一个思路。

用法:
    python check_assets.py [assets.json]
"""
import glob
import json
import os
import sys
from collections import defaultdict

# 允许共用主体的例外。每一条都要写明为什么共用是**有意的**。
ALLOW_SHARED = {
    # 地砖变体：同一枚举值的不同贴图，共用底模、只差染色，这正是设计意图
    "ground.glb": "Plain 系地砖变体 + Water，同一底模只差染色（见 README「地形三类」）",
    # 模块化塔件：Kenney 城堡包的塔分底座/中段/顶层数块，四座塔共用底座、
    # **靠顶部区分**（见 README；Flak 与 Tower 的剪影差异就在顶上）
    "tower-square-base.glb": "四座塔共用模块化底座，靠顶部区分（这是该素材包的设计方式）",
    "tower-square-mid.glb": "同上",
    "tower-square-top.glb": "同上",
    # ForestB 就是 Forest 枚举的贴图变体，机制相同，共用底模是定义本身
    "tree-large.glb": "ForestB 是 Forest 的贴图变体（单株巨树占一格），机制相同",
    # 跨包组合：同一匹马给两个骑兵，靠染色与骑手区分（活马 vs 幽灵马）
    "Horse.blend": "逐风猎骑与鬼域骑士团共用马模，靠染色区分活马 / 幽灵马",
    "Skeleton.blend": "亡灵步兵与亡灵弓手共用骷髅模，靠武器区分近战 / 远程",
}


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def entities(manifest):
    """-> [(标识符, [模型路径...])]，列表首项是主体。"""
    out = []
    for section in ("units", "buildings", "terrain", "obstacles"):
        for ident, spec in manifest.get(section, {}).items():
            # 分组内允许放 `_comment` 之类的说明，它们不是实体
            if ident.startswith("_"):
                continue
            if "model" in spec:
                models = [spec["model"]]
            else:
                models = [p["model"] for p in spec.get("parts", [])]
            out.append((ident, models))
    return out


def check(manifest_path):
    base = os.path.dirname(os.path.abspath(manifest_path))
    manifest = load(manifest_path)
    ents = entities(manifest)
    problems = []

    # 一、每个引用的模型文件都要存在。缺文件在渲染时才报，而那时已经跑了几分钟
    for ident, models in ents:
        for m in models:
            if not os.path.exists(os.path.join(base, m)):
                problems.append(f"{ident}: 模型不存在 —— {m}")

    # 二、不同实体不得共用主体模型
    owners = defaultdict(list)
    for ident, models in ents:
        if models:
            owners[os.path.basename(models[0])].append(ident)
    for model, who in sorted(owners.items()):
        if len(who) > 1 and model not in ALLOW_SHARED:
            problems.append(
                f"{model}: 被 {len(who)} 个实体当作**主体** —— {'、'.join(who)}\n"
                f"    两个机制不同的实体共用一张图，玩家在画面上分不出来。\n"
                f"    换一个模型，或在 ALLOW_SHARED 里写明为什么共用是有意的。"
            )

    # 三、配件用了别的实体的主体模型 —— **只警告，不失败**。
    #
    # 这一条与上一条不同，它有大量**正当**的情形：岩壁里含几块碎石、密林里含几棵小树，
    # 那是构成关系而不是误导。只有当配件大到能被读成一个独立实体时才是问题
    # （踩过的例：伐木场摆了几根 tree-trunk，而那是可破坏障碍 Stump 的主体，
    #   玩家会以为那几根能打穿）。而「大到能被读成」没法用规则判，只能人看。
    #
    # 所以它不进 problems。**一个总是红的检查等于没有检查**——人会开始无视它，
    # 连带把上面两条真正的硬失败一起无视掉。
    primaries = {os.path.basename(m[0]): i for i, m in ents if m}
    warnings = []
    for ident, models in ents:
        for m in models[1:]:
            b = os.path.basename(m)
            if b in primaries and primaries[b] != ident and b not in ALLOW_SHARED:
                warnings.append(f"{ident} 的配件用了 {primaries[b]} 的主体模型（{b}）")

    if warnings:
        print("提示（不失败）：以下配件与别的实体的主体同模。"
              "请目视确认它们小到不会被读成一个独立实体——\n")
        for w in warnings:
            print("  · " + w)
        print()

    if problems:
        print(f"assets.json 校验失败（{len(problems)} 处）：\n")
        for p in problems:
            print("  " + p + "\n")
        return 1
    print(f"assets.json 校验通过（{len(ents)} 个实体，"
          f"{sum(len(m) for _, m in ents)} 处模型引用）")
    return 0


def check_orphans(out_dir):
    """产物目录里有没有元数据不认识的 PNG（孤儿），以及元数据声明了却不存在的（缺失）。

    这条是踩过才加的：把某个状态的 `frames` 从 `[1,7,11,16]` 改成 `[1,7,13,19]` 之后，
    旧的 `_11` / `_16` 八个文件**留在磁盘上**——`--only` 重渲只覆盖同名文件、不删旧的。
    症状是纯静默的：元数据是对的、前端按元数据取图也是对的，
    只有**交付包会多带一批没人引用的图**，而那正是交付清单要求精简的东西。

    「缺失」那一半同样重要：`--only` 渲了一半中断时元数据已写、文件没齐，
    前端会在运行时报文件找不到，而那时人会先怀疑前端。
    """
    meta_path = os.path.join(out_dir, "_sprite_meta.json")
    if not os.path.exists(meta_path):
        print(f"跳过产物检查：{meta_path} 不存在（还没渲过）")
        return 0
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)

    want = set()
    for ident, spr in meta["sprites"].items():
        for st, info in spr["states"].items():
            frs = info["frames"]
            for d in meta["dirs"]:
                for fr in frs:
                    sfx = f"_{fr}" if len(frs) > 1 else ""
                    want.add(f"{ident}_{st}_{d}{sfx}.png")

    # 下划线开头的是汇总图 / 预览图 / 元数据本身，不属于精灵
    have = {os.path.basename(p) for p in glob.glob(os.path.join(out_dir, "*.png"))
            if not os.path.basename(p).startswith("_")}

    orphan, missing = sorted(have - want), sorted(want - have)
    if not orphan and not missing:
        print(f"产物检查通过（{len(want)} 张精灵，与元数据一致）")
        return 0
    if orphan:
        print(f"\n产物检查失败：{len(orphan)} 张 PNG 不在元数据里（孤儿）")
        for f in orphan[:12]:
            print("  · " + f)
        if len(orphan) > 12:
            print(f"  · …另有 {len(orphan) - 12} 张")
        print("  改过某个状态的 frames 之后要删掉旧帧号的文件；--only 重渲不会删。")
    if missing:
        print(f"\n产物检查失败：{len(missing)} 张元数据声明了但磁盘上没有")
        for f in missing[:12]:
            print("  · " + f)
    return 1


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "assets.json")
    rc = check(path)
    out = os.path.join(os.path.dirname(os.path.abspath(path)), "out_3d")
    return rc | check_orphans(out)


if __name__ == "__main__":
    sys.exit(main())
