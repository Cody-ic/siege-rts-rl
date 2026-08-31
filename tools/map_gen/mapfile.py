#!/usr/bin/env python3
"""地图文件的读写与 content_hash —— 仓库里唯一的地图 I/O 入口。

`地图与场景设计.md` 6.3 要求回放文件记录 `map_id` + `content_hash`，载入时不匹配
直接报错。这条的全部价值在于「地图改了一个格子，旧回放不会**静默**偏离」，
所以哈希算法必须在生成端与校验端完全一致 —— 两处各写一遍正是这类需求最典型的
失败方式。因此本模块是唯一入口：生成器写地图、校验器读地图、将来 `rts_core` 侧
对拍，都只经过这里的 `dumps()` / `loads()`。

## 关于「规范化」：这里做的比 6.3 字面要求更强

6.3 写的是「取除该字段本身以外全部内容、经规范化之后的 SHA-256」，
规范化 = 换行统一为 LF + 去掉每行尾随空白。

本模块**不对文件文本做规范化，而是对「删掉 content_hash 键之后重新规范序列化」
的结果取哈希**。理由是文本级规范化解决不了另外三种同样会改变字节的差异：

- **缩进宽度** —— 有人用编辑器顺手格式化了一下
- **键的顺序** —— 不同 json 库 / 不同版本的 dict 序列化顺序
- **非 ASCII 的转义** —— `ensure_ascii` 开或关，`"王国边境"` 与 `"\\u738b\\u56fd..."`

这三种的症状与 6.3 描述的 CRLF 那条**一模一样**：报「地图不匹配」，而看到它的人
第一反应一定是去查仿真。规范序列化把三种一次全消掉，而 LF 与无尾随空白只是它的
副产品 —— 序列化本来就不产生这两样，所以 6.3 要的东西自动满足。

代价是与 6.3 的字面表述不完全一致。建议 6.3 改写为「对**规范序列化**后的内容取
哈希」并把规范序列化的定义写进去（即下面的 `_CANONICAL_KWARGS`）。这一条随本 PR
一并提出，未定之前以本模块为准 —— 因为它是可执行的那一份。

## 坐标约定：`[x, y]`，而 `rows[y][x]`

6.2 的字段说明没有明写坐标顺序，但示例里四个集结点分别落在上 / 右 / 下 / 左边缘
（`[48,2]` `[94,48]` `[48,94]` `[2,48]`），只有按 `[x, y]` 读才讲得通。
`layers.*.rows` 是按**行**编码的，所以 `rows[y][x]` 才是 `pos=[x,y]` 那一格。

这条容易写反且反了以后在方形地图上不会立刻报错（只是整张图转置了），
所以在这里写死并由 `test_mapfile.py` 用非方形地图钉住。
"""
import hashlib
import json

# 规范序列化的定义。改动它等于改动全部已入库地图的 content_hash，
# 属于跨模块契约变更（回放文件头依赖它），不要顺手动。
#
# - sort_keys：消掉键顺序差异
# - ensure_ascii=False：中文按 UTF-8 原样写，不转义成 \uXXXX。
#   两个理由：地图名要人能读、以及 6.1 要求「逐行可 diff」
# - indent=2：固定缩进，同时保住 6.1 的「一行地图 = 一行文本」
# - separators：去掉 json 默认在逗号后留的尾随空格。不指定的话 indent 模式下
#   每个 `,` 后面跟一个空格，而那正好是 6.3 要去掉的「行尾空白」的一种
_CANONICAL_KWARGS = dict(
    sort_keys=True,
    ensure_ascii=False,
    indent=2,
    separators=(",", ": "),
)

HASH_FIELD = "content_hash"
HASH_PREFIX = "sha256:"

# 地形调色板。顺序即 rows 里的字符编码（'0' = palette[0]），与 4.1 的枚举一致。
# rows 用单字符编码，所以调色板最多 10 项 —— 五种地形远够，但越界要报错而不是
# 悄悄用上 'a'、'b' 这类字符，那会让 rows 变成一个没人看得懂的东西。
TERRAIN_PALETTE = ["Plain", "Rock", "Forest", "Water", "Bridge"]

# 上面那句注释说「越界要报错」，而在加这行之前它并不会报。
# `_check_rows` 用的允许字符集是 `{str(i) for i in range(len(TERRAIN_PALETTE))}`，
# 第 11 项时集合里放进去的是**两字符**的 "10"，而 `set(row)` 里全是单字符，
# 于是它永远匹配不上：第 11 种地形既编不进去、也不会有任何东西红。
# 地形枚举这一周刚从 3 变 5，这不是纯理论。
assert len(TERRAIN_PALETTE) <= 10, \
    "rows 是单字符编码，调色板超过 10 项要先改编码（见 6.1）"

# `layers` 只认这两层，多出来的一律报错而不是忽略 —— 理由见 check_format。
KNOWN_LAYERS = frozenset({"terrain", "no_build"})

RESOURCE_TYPES = {"stone", "wood", "gold"}
RESOURCE_TIERS = {"inner", "outer"}
WALL_KINDS = {"Wall", "Gate"}

# 地图可预置的**非 Keep / Wall / Gate** 建筑（2026-08-31 新增，随本次地图重设计）。
# `Keep` 走专门的 `keep` 字段（World 要求恰好一座），`Wall`/`Gate` 走 `initial_walls`
# （带 `hp_frac` 残血，2.3 的设计要求），两者都不进这份列表——重复表达同一件事
# 只会制造「两个字段互相不一致」的新洞。这份列表管的是"开局时玩家已经拥有的其余建筑"
# （箭塔、兵营、伐木场、采石场……），此前地图 schema 完全没有承载它的地方。
# 键取花名册标识符原样大小写，同 `WALL_KINDS`/`kObstacleTypes` 的风格
# （这两张表的键是实体标识符，不是类别名）。
BUILDING_TYPES = {"Tower", "Flak", "Watch", "Barrack", "Fence", "Quarry", "Lumber", "Mine"}

# 6.2 的 corridor 是枚举而非自由字符串（校验器第 3 条要查每种恰好一次）。
# 取值来自 2.2 的走廊候选清单。**清单本身是候选、不是定数**（2.2 明写），
# 所以这里只用于拼写检查，不用于「必须四种都有」——那条属校验器第 3 条，
# 且它查的是「种类数 = 集结点数」，不是「等于 4」。
CORRIDOR_KINDS = {"open", "defile", "forest", "economy"}


class MapFormatError(ValueError):
    """地图文件在**格式**层面就不合法（字段缺失、行长对不上、坐标越界……）。

    与校验器第 8 节的那 15 条是两回事：那 15 条查的是「这张图设计得对不对」，
    本异常查的是「这份文件读不读得成一张图」。前者失败说明地图要改，
    后者失败说明文件坏了或者写它的代码有 bug。
    """


def canonical_dumps(doc):
    """按规范序列化输出，结尾带一个换行。

    结尾换行是刻意的：POSIX 文本文件约定以换行结尾，缺了它 `git diff` 会显示
    「\\ No newline at end of file」，而 6.1 要求地图逐行可 diff。
    它进哈希，所以两端必须一致 —— 这也是为什么写文件与算哈希都只走这一个函数。
    """
    return json.dumps(doc, **_CANONICAL_KWARGS) + "\n"


def compute_content_hash(doc):
    """算 content_hash：删掉该字段本身，规范序列化，取 UTF-8 字节的 SHA-256。"""
    without = {k: v for k, v in doc.items() if k != HASH_FIELD}
    payload = canonical_dumps(without).encode("utf-8")
    return HASH_PREFIX + hashlib.sha256(payload).hexdigest()


def verify_content_hash(doc):
    """返回 (是否一致, 文件里写的, 实际算出的)。

    不抛异常是有意的：校验器要把它作为第 15 条报告出来，而不是让整个流程炸掉。
    """
    actual = compute_content_hash(doc)
    stated = doc.get(HASH_FIELD)
    return stated == actual, stated, actual


def stamp_content_hash(doc):
    """就地写入 content_hash 并返回 doc。写地图前必须调一次。"""
    doc[HASH_FIELD] = compute_content_hash(doc)
    return doc


def dumps(doc, stamp=True):
    """序列化成文本。默认先补上 content_hash —— 忘记补是最容易犯的错。"""
    if stamp:
        doc = stamp_content_hash(dict(doc))
    return canonical_dumps(doc)


def loads(text, check_structure=True):
    """解析文本。默认做格式层校验，不做第 8 节的语义校验。"""
    try:
        doc = json.loads(text)
    except json.JSONDecodeError as e:
        raise MapFormatError(f"不是合法的 JSON：{e}") from e
    if not isinstance(doc, dict):
        raise MapFormatError(f"顶层必须是对象，实际是 {type(doc).__name__}")
    if check_structure:
        check_format(doc)
    return doc


def load(path, check_structure=True):
    """读地图。**显式指定 UTF-8**，不吃平台默认编码。

    不指定的话 Windows 中文环境下 open() 默认走 GBK，地图名一有中文就炸，
    而错误信息（UnicodeDecodeError）完全不指向真正的原因。
    换行交给 newline=None 的通用换行模式统一成 \\n，于是工作区里是 CRLF 还是 LF
    都读得成同一个 doc —— 这是 6.3 那条跨平台问题在读取侧的另一半。
    """
    with open(path, "r", encoding="utf-8") as f:
        return loads(f.read(), check_structure=check_structure)


def save(path, doc, stamp=True):
    """写地图。**newline="\\n" 是必须的**，否则 Windows 上 Python 会把 \\n 翻成 \\r\\n。

    翻了之后文件字节与 canonical_dumps() 的输出不同，于是「写完再读回来算哈希」
    仍然一致（读取时通用换行又翻回来了），但**文件本身在两个平台上字节不同**。
    `.gitattributes` 的 `*.json text eol=lf` 是第二道防线，这里是第一道。
    """
    text = dumps(doc, stamp=stamp)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return text


# --------------------------------------------------------------------------
# 格式层校验
# --------------------------------------------------------------------------

def _need(doc, key, types, where="顶层"):
    if key not in doc:
        raise MapFormatError(f"{where}缺字段 `{key}`")
    v = doc[key]
    if not isinstance(v, types):
        names = types.__name__ if isinstance(types, type) else "/".join(
            t.__name__ for t in types)
        raise MapFormatError(
            f"{where}的 `{key}` 应是 {names}，实际是 {type(v).__name__}")
    return v


def _need_pos(v, where, size):
    """坐标必须是 [x, y] 两个界内整数。

    bool 是 int 的子类，所以要显式排掉 —— `[True, 0]` 不该被当成 `[1, 0]` 收下。
    """
    if (not isinstance(v, list) or len(v) != 2
            or any(isinstance(c, bool) or not isinstance(c, int) for c in v)):
        raise MapFormatError(f"{where} 应是 [x, y] 两个整数，实际是 {v!r}")
    x, y = v
    w, h = size
    if not (0 <= x < w and 0 <= y < h):
        raise MapFormatError(f"{where} 的坐标 [{x}, {y}] 越界（地图 {w}×{h}）")
    return x, y


def _check_rows(layer, name, size, allowed_chars):
    w, h = size
    rows = _need(layer, "rows", list, where=f"`layers.{name}`")
    if len(rows) != h:
        raise MapFormatError(
            f"`layers.{name}.rows` 有 {len(rows)} 行，但 size 说高度是 {h}")
    for y, row in enumerate(rows):
        if not isinstance(row, str):
            raise MapFormatError(
                f"`layers.{name}.rows[{y}]` 应是字符串，实际是 {type(row).__name__}")
        if len(row) != w:
            raise MapFormatError(
                f"`layers.{name}.rows[{y}]` 长 {len(row)}，但 size 说宽度是 {w}")
        bad = sorted(set(row) - allowed_chars)
        if bad:
            raise MapFormatError(
                f"`layers.{name}.rows[{y}]` 出现未定义字符 {bad}；"
                f"本层只认 {sorted(allowed_chars)}")


def check_format(doc):
    """格式层校验。通过它只说明「这份文件读得成一张图」，不说明这张图合理。

    第 8 节那 15 条语义校验在 validate.py（PR 2），两者刻意分开：
    生成器每生成一张都要先能读成图才谈得上校验，把两者混在一起会让
    「文件坏了」和「地图设计不合格」报出同一种错。
    """
    _need(doc, "format", int)
    if doc["format"] != 1:
        raise MapFormatError(f"不认识的 format 版本 {doc['format']}，本工具只支持 1")

    _need(doc, "map_id", str)
    _need(doc, "name", str)

    size = _need(doc, "size", list)
    if (len(size) != 2
            or any(isinstance(c, bool) or not isinstance(c, int) for c in size)
            or any(c <= 0 for c in size)):
        raise MapFormatError(f"`size` 应是 [宽, 高] 两个正整数，实际是 {size!r}")

    layers = _need(doc, "layers", dict)
    unknown = sorted(set(layers) - KNOWN_LAYERS)
    if unknown:
        raise MapFormatError(
            f"`layers` 里有未知的层 {unknown}，本工具只认 {sorted(KNOWN_LAYERS)}。\n"
            "  拒绝而不是忽略：`no_bulid` 这类拼写错误若被静默忽略，那一层就当作"
            "不存在（相当于全 0），而地图看起来完全正常 —— 又是一处「该红却绿」。\n"
            "  若这是新进契约的字段（例如候选乙的 city mask），"
            "需要同步更新 KNOWN_LAYERS 与 check_format。")
    terrain = _need(layers, "terrain", dict, where="`layers`")
    palette = _need(terrain, "palette", list, where="`layers.terrain`")
    if palette != TERRAIN_PALETTE:
        raise MapFormatError(
            f"`layers.terrain.palette` 必须正好是 {TERRAIN_PALETTE}（顺序也要一致，"
            f"因为 rows 里的字符是它的下标），实际是 {palette!r}")
    _check_rows(terrain, "terrain", size,
                {str(i) for i in range(len(TERRAIN_PALETTE))})

    no_build = _need(layers, "no_build", dict, where="`layers`")
    _check_rows(no_build, "no_build", size, {"0", "1"})

    _need_pos(_need(doc, "keep", list), "`keep`", size)

    spawns = _need(doc, "spawns", list)
    if not spawns:
        raise MapFormatError("`spawns` 不能为空 —— 没有集结点就没有进攻方")
    seen_ids = set()
    for i, s in enumerate(spawns):
        where = f"`spawns[{i}]`"
        if not isinstance(s, dict):
            raise MapFormatError(f"{where} 应是对象")
        sid = _need(s, "id", int, where=where)
        if sid in seen_ids:
            raise MapFormatError(f"{where} 的 id={sid} 与前面的重复")
        seen_ids.add(sid)
        _need_pos(_need(s, "pos", list, where=where), f"{where}.pos", size)
        corridor = _need(s, "corridor", str, where=where)
        if corridor not in CORRIDOR_KINDS:
            raise MapFormatError(
                f"{where}.corridor = {corridor!r} 不在候选清单 "
                f"{sorted(CORRIDOR_KINDS)} 里")

    resources = _need(doc, "resources", list)
    for i, r in enumerate(resources):
        where = f"`resources[{i}]`"
        if not isinstance(r, dict):
            raise MapFormatError(f"{where} 应是对象")
        rtype = _need(r, "type", str, where=where)
        if rtype not in RESOURCE_TYPES:
            raise MapFormatError(
                f"{where}.type = {rtype!r} 不是 {sorted(RESOURCE_TYPES)} 之一")
        tier = _need(r, "tier", str, where=where)
        if tier not in RESOURCE_TIERS:
            raise MapFormatError(
                f"{where}.tier = {tier!r} 不是 {sorted(RESOURCE_TIERS)} 之一")
        _need_pos(_need(r, "pos", list, where=where), f"{where}.pos", size)
        # **必填、与 `tier` 不相关**（地图与场景设计.md 6.2/10）：`tier` 只是描述、
        # 不影响仿真；`unlock_wave` 影响仿真（哪一波之后这个点才产出），两者
        # 不能互相替代。下界 1——`rts::World::wave()` 从 1 起。
        wave = _need(r, "unlock_wave", int, where=where)
        if isinstance(wave, bool) or wave < 1:
            raise MapFormatError(
                f"{where}.unlock_wave = {wave!r} 必须是 ≥ 1 的整数")

    walls = _need(doc, "initial_walls", list)
    for i, wseg in enumerate(walls):
        where = f"`initial_walls[{i}]`"
        if not isinstance(wseg, dict):
            raise MapFormatError(f"{where} 应是对象")
        kind = _need(wseg, "kind", str, where=where)
        if kind not in WALL_KINDS:
            raise MapFormatError(
                f"{where}.kind = {kind!r} 不是 {sorted(WALL_KINDS)} 之一")
        _need_pos(_need(wseg, "pos", list, where=where), f"{where}.pos", size)
        hp = _need(wseg, "hp_frac", (int, float), where=where)
        if isinstance(hp, bool) or not (0.0 < hp <= 1.0):
            raise MapFormatError(
                f"{where}.hp_frac = {hp!r} 应落在 (0, 1]；"
                f"0 表示墙已经没了，那种情况应当直接不写这条")

    # 玩家开局已拥有的其余建筑（2026-08-31 新增）。**必填，空数组也要写出来**——
    # 与 `obstacles` 同一条纪律：缺失与刻意为空不可区分，会让新的摆放冲突检查
    # （第 24 条）在"这张图没有这个字段"的情况下无法区分"没有初始建筑"与
    # "写图的人不知道有这个字段"。**不带血量字段**：满血进场，同 `ObstacleNode`
    # ——没有任何设计要求说玩家的初始建筑开局就该带伤（与 2.3 只约束 `initial_walls`
    # 残破不同，那是"城圈必须残破"这一条独立要求，不延伸到其余建筑）。
    buildings = _need(doc, "buildings", list)
    for i, b in enumerate(buildings):
        where = f"`buildings[{i}]`"
        if not isinstance(b, dict):
            raise MapFormatError(f"{where} 应是对象")
        btype = _need(b, "type", str, where=where)
        if btype not in BUILDING_TYPES:
            raise MapFormatError(
                f"{where}.type = {btype!r} 不是 {sorted(BUILDING_TYPES)} 之一"
                f"（`Keep` 走 `keep` 字段、`Wall`/`Gate` 走 `initial_walls`，"
                f"不出现在这里）")
        _need_pos(_need(b, "pos", list, where=where), f"{where}.pos", size)

    if HASH_FIELD in doc:
        v = doc[HASH_FIELD]
        if not isinstance(v, str) or not v.startswith(HASH_PREFIX):
            raise MapFormatError(
                f"`{HASH_FIELD}` 应是 {HASH_PREFIX}<hex> 形式，实际是 {v!r}")

    return doc
