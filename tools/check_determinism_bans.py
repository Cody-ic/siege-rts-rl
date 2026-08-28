#!/usr/bin/env python3
"""扫 rts_core/ 里破坏确定性的写法，作为一条 ctest。

CLAUDE.md 里有几条硬性禁令——禁 `rand()`、禁依赖未定序容器的遍历影响仿真结果、
禁拿地址当 key 或排序依据。问题是它们目前只是文字：违反了不会有任何反应，
直到某次回放对不上，而那时人会先怀疑仿真逻辑，最后才想到是遍历顺序变了。

这个脚本把那几条变成构建时就会红的检查。这是「结构封死，而非数值劝退」用在代码库上：
不指望每个人都记得规则，让违反规则的代码过不了测试。

**它是一条粗筛，不是解析器。** 只做注释与字符串剥离 + 正则匹配，不理解 C++ 语法。
已知漏网的一类是**跨行的声明**：匹配是逐行做的，所以被 clang-format 拆成
`std::map<\n    Unit*, int>` 的写法逃得掉。
误报时在该行末尾加 `// determinism-ok: <理由>` 放行——刻意要求写理由，
这样放行是可评审的，而不是悄悄绕过。

用法:
    python check_determinism_bans.py <目录或文件> [更多...]
    python check_determinism_bans.py --self-test
"""
import argparse
import re
import sys
from pathlib import Path

EXTS = {".h", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx"}

OK_MARKER = "determinism-ok"

# (正则, 说明)。说明会直接打给违规者看，所以要写清「为什么这条不行」。
BANS = [
    (r"\bunordered_(map|set|multimap|multiset)\b",
     "无序容器的迭代顺序随实现与地址变化。若它参与驱动仿真遍历，回放会静默偏离。"
     "用有序容器或按整型 ID 索引的扁平数组。"),
    # 允许 std::rand 这种带限定的写法，但不匹配 obj.rand(...) 这类成员调用（会误报）。
    (r"\bstd::s?rand\s*\(|(?<![\w.:>])s?rand\s*\(",
     "C 库全局随机函数没有可播种的独立状态，也无法随回放保存。用 rts::Rng。"),
    (r"\brandom_device\b",
     "random_device 取的是系统熵，同一种子无法复现。用 rts::Rng。"),
    (r"\b(mt19937(_64)?|minstd_rand0?|ranlux\d+|knuth_b|default_random_engine)\b",
     "标准库随机引擎不入回放状态，且其分布适配器的实现是各标准库自定的。用 rts::Rng。"),
    # 逐个列出标准库的分布名，不用 \w+_distribution ——后者会把 type_distribution
    # 这类我们自己会写的自然命名（宏观层的兵种配比就是一个分布）全部误报。
    (r"\b(uniform_int|uniform_real|bernoulli|binomial|negative_binomial|geometric"
     r"|poisson|exponential|gamma|weibull|extreme_value|normal|lognormal|chi_squared"
     r"|cauchy|fisher_f|student_t|discrete|piecewise_constant|piecewise_linear)"
     r"_distribution\b",
     "标准库分布适配器的实现是各标准库自定的，MSVC 与 libstdc++ 对同一引擎同一种子"
     "给出不同数列。用 rts::Rng 的 below() / unit_float()。"),
    (r"\b(std::shuffle|random_shuffle)\b",
     "洗牌的结果取决于所用的分布实现。若确实需要，自己用 rts::Rng 写 Fisher-Yates。"),
    (r"\b(system_clock|steady_clock|high_resolution_clock)\b",
     "仿真是固定 20 Hz tick，不读挂钟。挂钟进入仿真即不可复现。"),
    # 只拦带限定的 std::time / std::clock 与 time_t；不拦裸 time( ——
    # 我们自己极可能有 sim.time() 这样的访问器。
    (r"\bstd::(time|clock)\s*\(|\btime_t\b",
     "同上：仿真内不读挂钟。"),
    # 键为指针的有序容器：只看第一个模板参数，避免把 map<int, Unit*> 当成违规。
    (r"\b(?:std::)?map\s*<\s*[^,<>;]*\*\s*,",
     "以指针为 key，遍历顺序随地址变化——这正是 CLAUDE.md 要靠扁平数组 + 整型 ID "
     "从结构上消除的那一类 bug。改用 UnitId / BldId。"),
    (r"\b(?:std::)?set\s*<\s*[^,<>;]*\*\s*>",
     "同上：以指针为元素的有序集合，遍历顺序随地址变化。"),
]

COMPILED = [(re.compile(pat), why) for pat, why in BANS]


def is_digit_separator(text, i):
    """text[i] 是 `'` 时，判断它是 C++14 的数字分隔符（`1'000`）还是字符字面量的起引号。

    不做这个区分的话，`'` 会被当成字面量起点、往后扫不到配对的引号、撞上换行才停，
    于是**该行剩下的内容全被丢掉**——`constexpr int k = 1'000; int x = rand();`
    里的 rand() 就查不出来。本项目将来会有一大批造价、血量、tick、地图尺寸常量，
    写成 `1'200` 是最自然的写法，所以这不是刁钻构造。

    判据：往前找出这一串由字母数字与 `'` 组成的 token 的开头，以数字开头才是数值
    字面量。这样能排除 `L'x'` 与 `u8'y'` —— 后者前一个字符是数字 `8`，光看前一个
    字符会误判。
    """
    n = len(text)
    if i == 0 or i + 1 >= n:
        return False
    if not text[i - 1].isalnum() or not text[i + 1].isalnum():
        return False
    k = i - 1
    while k > 0 and (text[k - 1].isalnum() or text[k - 1] == "'"):
        k -= 1
    return text[k].isdigit()


def strip_noncode(text):
    """剥掉注释与字符串字面量的内容，保留行数与列的大致位置。

    返回 (剥离后的文本, 放行行号集合)。放行标记本身在注释里，所以要在剥离前先摘出来。
    """
    allow = set()
    for lineno, line in enumerate(text.splitlines(), start=1):
        if OK_MARKER in line:
            allow.add(lineno)

    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            # 行注释：吃到行尾，换行留下以保住行号。
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif two == "/*":
            j = text.find("*/", i + 2)
            chunk = text[i:n] if j < 0 else text[i:j + 2]
            out.append("\n" * chunk.count("\n"))   # 只保留换行，内容丢掉
            i = n if j < 0 else j + 2
        elif text[i] == "'" and is_digit_separator(text, i):
            # `1'000` 里的 `'`：普通字符，不是字面量起点。
            out.append(text[i])
            i += 1
        elif text[i] in "\"'":
            # 字符串 / 字符字面量：整段替换成空，注意反斜杠转义。
            quote = text[i]
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                if text[j] == "\n":       # 未闭合，别把整个文件吃掉
                    break
                j += 1
            out.append(quote * 2)
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out), allow


def scan_text(text, label):
    code, allow = strip_noncode(text)
    hits = []
    for lineno, line in enumerate(code.splitlines(), start=1):
        if lineno in allow:
            continue
        for rx, why in COMPILED:
            m = rx.search(line)
            if m:
                hits.append((label, lineno, m.group(0), why))
    return hits


def iter_sources(targets):
    for t in targets:
        p = Path(t)
        if p.is_file():
            yield p
        elif p.is_dir():
            for f in sorted(p.rglob("*")):
                if f.is_file() and f.suffix.lower() in EXTS:
                    yield f
        else:
            print(f"跳过（不存在）: {p}", file=sys.stderr)


def self_test():
    """确认这个检查本身没有退化成永远绿的空测试。

    没有这条的话，正则写坏、剥离逻辑吃掉全部代码之类的失误都不会被发现——
    检查会一直通过，而我们会以为规则在生效。
    """
    must_flag = [
        "std::unordered_map<int, int> m;",
        "int x = rand();",
        "int y = std::rand();",
        "std::mt19937 gen(1);",
        "std::uniform_int_distribution<int> d(0, 5);",
        "std::map<Unit*, int> byptr;",
        "std::set<Unit*> s;",
        "auto t0 = std::chrono::steady_clock::now();",
        "std::time_t now = std::time(nullptr);",
        # 数字分隔符不得吃掉行内其余部分（下面三行只差那个 `'` 的位置与前缀）。
        "constexpr int k = 1'000; int x = rand();",
        "constexpr int m = 0xFF'FF; std::unordered_map<int, int> u;",
        # 字符字面量的剥离仍要正常工作：这两行的 rand() 在字面量之后，必须报出来。
        "char c = 'x'; int r = rand();",
        "wchar_t w = L'y'; int r = rand();",
    ]
    must_pass = [
        "// 这里解释为什么不能用 unordered_map 与 rand()",
        "/* 块注释里提到 std::mt19937 也不该报 */",
        'const char* msg = "unordered_map";',
        "std::map<int, Unit*> byid;",          # 指针是 value，不是 key
        "std::vector<Unit*> v;",              # vector 按下标遍历，顺序确定
        "Tick t = sim.time();",               # 自己的访问器，不是 std::time
        "auto mix = type_distribution(budget);",   # 兵种配比也叫 distribution
        "std::uint32_t r = rng_.below(6);",   # 我们自己的 PRNG
        "std::unordered_map<int, int> cache;  // determinism-ok: 仅用于离线工具，不进仿真",
        "constexpr int hp = 1'200;",          # 数字分隔符本身不是违规
        "char8_t c8 = u8'z';",                # 前一个字符是数字 8，但这是字符字面量
        'const char* s = "1\'000 rand()";',   # 字符串里的分隔符与禁词都不算
    ]
    failures = []
    for src in must_flag:
        if not scan_text(src, "<self-test>"):
            failures.append(f"应当报却没报: {src}")
    for src in must_pass:
        hits = scan_text(src, "<self-test>")
        if hits:
            failures.append(f"误报: {src}  ->  命中 {hits[0][2]!r}")
    if failures:
        print("自检失败：")
        for f in failures:
            print("  " + f)
        return 1
    print(f"自检通过（{len(must_flag)} 条应报、{len(must_pass)} 条不应报）")
    return 0


def main():
    ap = argparse.ArgumentParser(description="扫破坏确定性的写法")
    ap.add_argument("targets", nargs="*", help="要扫的目录或文件")
    ap.add_argument("--self-test", action="store_true", help="只自检，不扫源码")
    a = ap.parse_args()

    if a.self_test:
        return self_test()
    if not a.targets:
        ap.error("需要至少一个目录或文件，或用 --self-test")

    # 路径写错必须红。原先只往 stderr 打一句"跳过（不存在）"然后照样返回 0，
    # 于是目录改名、或将来把 rts_core/ 拆成几个子目录忘了改 add_test，
    # 这条 ctest 会**静默通过、什么都没检查**——正是这份脚本要防的那种退化。
    missing = [t for t in a.targets if not Path(t).exists()]
    if missing:
        print("确定性检查失败：以下路径不存在 —— " + "、".join(missing))
        print("    这条检查的全部价值在于它真的扫过代码，所以路径错时必须红，"
              "不能报「通过（0 个文件）」。")
        return 1

    hits, n_files = [], 0
    for f in iter_sources(a.targets):
        n_files += 1
        hits += scan_text(f.read_text(encoding="utf-8"), str(f))

    # 同上：一个源文件都没扫到，只能说明参数或目录结构变了，不能算通过。
    if n_files == 0:
        print("确定性检查失败：一个源文件都没扫到（目标存在但里面没有 C/C++ 源文件）。")
        print(f"    目标：{'、'.join(a.targets)}")
        print(f"    认的后缀：{'、'.join(sorted(EXTS))}")
        return 1

    if not hits:
        print(f"确定性检查通过（扫了 {n_files} 个源文件）")
        return 0

    print(f"发现 {len(hits)} 处破坏确定性的写法：\n")
    for label, lineno, token, why in hits:
        print(f"{label}:{lineno}: {token}")
        print(f"    {why}")
        print(f"    确实是误报的话，在该行末尾加  // {OK_MARKER}: <理由>\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
