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
    # 模块化塔件：Kenney 城堡包的塔分底座/中段/顶层数块，靠顶部区分
    # （见 README；Flak 与 Tower 的剪影差异就在顶上）。
    #
    # **共用底座只对「都是砌体战斗塔」的那几座成立。** Watch 曾经也在这一列，
    # 靠把同一套砌体染棕来表示木质——那是无效的，染色改不掉「实心砌体」这个形体。
    # 它现在改用镂空木架，因此不在此列。这条例外的适用边界是**机制同族**，
    # 不是「反正都是塔」。
    #
    # **`Barrack` 曾经也在这一列，2026-08-30 移出。** 它零战力，而这一族的定义是
    # 「有火力」——把它算进来正是「太像」的根因（评审直接指出）。现在它一件构件都
    # 不与人共用（`mid-door` + `top-roof-high-windows` 都是独占），所以**这里不必给它
    # 留位置**。留着一条用不到的豁免比没有更坏：它会在将来真的共用时静默放行。
    "tower-square-base.glb": "Tower / Flak 两座砌体战斗塔共用模块化底座，靠顶部区分",
    "tower-square-top.glb": "同上（Watch 用它作开放平台，是配件不是主体）",
    # ForestB 就是 Forest 枚举的贴图变体，机制相同，共用底模是定义本身
    "tree-large.glb": "ForestB 是 Forest 的贴图变体（单株巨树占一格），机制相同",
    # 跨包组合：同一匹马给两个骑兵，靠染色与骑手区分（活马 vs 幽灵马）
    "Horse.blend": "逐风猎骑与鬼域骑士团共用马模，靠染色区分活马 / 幽灵马",
    # **`Skeleton.blend` 曾经也在这一列，2026-08-30 移出。** 原文是「亡灵步兵与
    # 亡灵弓手共用骷髅模，靠武器区分近战 / 远程」，而 `Shade` 换成披袍法师
    # （`Wizard.blend`）之后，拿它当**主体**的只剩 `Ghoul` 一个——豁免用不到了。
    #
    # 摘掉它的理由与同一批里 `Barrack` 那条**逐字相同**：留着一条用不到的豁免比
    # 没有更坏，它会在将来真的共用时静默放行。而这一条是漏掉的——同一个判据在
    # 同一个 PR 里应用了一次、放过了一次。**判据不会自己去找它的全部适用处**：
    # 改一个实体的素材时，要回头搜它的标识符还留在哪些**规则**里，而不只是哪些文字里。
    #
    # 摘掉的副作用是 `Knight` 的骷髅骑手（parts[1]）从「被静默」变成第三条那张
    # 目视清单上的一行——那正是它该在的地方（骑手是构成关系，与「岩壁里含几块碎石」同类）。
}


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def entities(manifest):
    """-> [(标识符, [模型路径...])]，列表首项是主体。"""
    out = []
    for section in ("units", "buildings", "terrain", "obstacles", "projectiles"):
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
        # **朝向集合按实体取。** `meta["dirs"]` 只是默认值，自带 `dirs` 的实体
        # （弹丸只有 `FREE`）以它为准。用全局那一份的话，这个检查会**同时**
        # 报两遍同一件事：`Arrow_idle_FREE.png` 成了「孤儿」、
        # 四张 `Arrow_idle_SE/SW/NE/NW.png` 成了「缺失」——
        # 而真相是它们本来就不该存在。
        dirs = spr.get("dirs", meta["dirs"])
        for st, info in spr["states"].items():
            frs = info["frames"]
            for d in dirs:
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


def check_projectiles(out_dir):
    """弹丸必须**横躺**、且 `pivot` 必须落在图上。

    两条都是「静态图上看不出来、转起来才暴露」的那类，所以只能靠检查守。

    **横躺**：`dirs: ["FREE"]` 的整个前提是前端按飞行角 2D 旋转，而旋转的
    起点必须是水平（屏幕 +X = 0 弧度）。若哪天有人改了 `yaw` 或 part 的 `rot`
    让箭变斜，前端画出来就是「箭大致朝目标飞、但总歪一个固定角度」——
    那看着像弹道算错，不像资产不对。判据取内容框宽高比：横躺的箭实测 9.3、
    弩矢 5.1，而斜 45° 的话会掉到 2 以下。阈值 3.0 两边都有余量。

    **但这一条只对「有朝向」的弹丸成立**（元数据 `oriented`）。魔法弹是个球，
    旋转它是恒等变换，「躺平」对它没有定义、宽高比恒为 1.0——照搬这条检查
    等于永远红。**这不是给它开豁免，是这条检查的适用范围本来就是有朝向的那一类。**
    差别是实质的：豁免要靠人记得为什么放行，而 `oriented: false` 是一条
    会被检查读到的事实，写错了（把箭标成无朝向）只会让检查变松，不会误报。

    **pivot 落在图上**：它是几何 bbox 中心的投影，而 alpha 内容框中心是
    渲染出来的实际中心，两者只该差抗锯齿那点量（实测 0.4 与 1.9 px）。
    差多了说明 `mesh_center_px()` 与实际渲的不是同一批网格。
    """
    meta_path = os.path.join(out_dir, "_sprite_meta.json")
    if not os.path.exists(meta_path):
        return 0
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    try:
        from PIL import Image
    except ImportError:
        print("跳过弹丸检查：没有 Pillow")
        return 0

    RATIO_MIN, PIVOT_TOL = 3.0, 4.0
    bad, n, n_or = [], 0, 0
    for ident, spr in meta["sprites"].items():
        for st, info in spr["states"].items():
            if info.get("kind") != "projectile":
                continue
            for d in spr.get("dirs", meta["dirs"]):
                frs = info["frames"]
                for fr in frs:
                    sfx = f"_{fr}" if len(frs) > 1 else ""
                    fn = f"{ident}_{st}_{d}{sfx}.png"
                    p = os.path.join(out_dir, fn)
                    if not os.path.exists(p):
                        continue          # 缺图由 check_orphans 报，这里不重复
                    n += 1
                    bb = Image.open(p).convert("RGBA").split()[3].getbbox()
                    if bb is None:
                        bad.append(f"{fn}: 整张全透明")
                        continue
                    w, h = bb[2] - bb[0], bb[3] - bb[1]
                    ratio = w / max(h, 1)
                    if info.get("oriented", True):
                        n_or += 1
                    if info.get("oriented", True) and ratio < RATIO_MIN:
                        bad.append(f"{fn}: 内容框 {w}x{h}，宽高比 {ratio:.2f} < "
                                   f"{RATIO_MIN}——没有横躺，前端的 2D 旋转会带一个"
                                   f"固定偏角")
                    pv = info.get("pivot")
                    if pv is None:
                        bad.append(f"{fn}: kind 是 projectile 但元数据没有 pivot")
                        continue
                    cx, cy = (bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2
                    dx, dy = abs(pv[0] - cx), abs(pv[1] - cy)
                    if dx > PIVOT_TOL or dy > PIVOT_TOL:
                        bad.append(f"{fn}: pivot {pv} 离内容框中心 "
                                   f"({cx:.1f},{cy:.1f}) 差 ({dx:.1f},{dy:.1f})，"
                                   f"超过 {PIVOT_TOL} px——箭会绕一个偏离自身的点转")
    if bad:
        print(f"\n弹丸检查失败：{len(bad)} 处")
        for b in bad:
            print("  · " + b)
        return 1
    if n:
        # **两个数字都要报。** 只说「N 张通过」的话，把某张误标成 `oriented: false`
        # 会让它悄悄退出横躺检查而输出一字不变——那正是这条检查要防的失效方式，
        # 却由它自己的提示语掩盖掉。报出「其中几张受横躺检查」，改动就看得见。
        print(f"弹丸检查通过（{n} 张，pivot 均落在图上；其中 {n_or} 张受横躺检查，"
              f"另 {n - n_or} 张标了 oriented: false（球状，旋转是恒等变换））")
    return 0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "assets.json")
    rc = check(path)
    out = os.path.join(os.path.dirname(os.path.abspath(path)), "out_3d")
    return rc | check_orphans(out) | check_projectiles(out)


if __name__ == "__main__":
    sys.exit(main())
