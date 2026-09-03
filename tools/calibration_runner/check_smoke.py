#!/usr/bin/env python3
"""calibration_runner 的 ctest 冒烟：跑真实二进制，钉住输出格式与关键字段。

这条 ctest 不跑全量 20 波——字典序第一张池图、一个种子、2 波上限、6000 tick
上限（`--limit 1`），只钉两件事：

1. **链路通**：二进制退出码 0、产出能被 json 解析（`schema == "calibration_runner/1"`）。
2. **字段在**：run / wave 记录里 §7 点名的每一项都在，且基本自洽
   （波长 = 首末 tick 差、开打晚于开波、堡垒初始血量 > 0、第 1 波有 Ghoul）。

断言只查**形状与自洽**，不查具体数值——具体数值随数值表与地图池变，钉死它们
等于每轮标定都要来改测试。真要防的是「输出格式静默改坏、下游分析脚本全瞎」，
那是没有测试时必然发生、又最晚被发现的一类坏。

用法:
    python check_smoke.py <calibration_runner 可执行文件> <地图目录> <输出 JSON 路径>
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runner", help="calibration_runner 可执行文件路径")
    ap.add_argument("maps_dir", help="池图目录（UTF-8 路径，原样传给二进制）")
    ap.add_argument("out_json", help="冒烟输出 JSON 的落盘路径")
    args = ap.parse_args()

    # --limit 1：只跑字典序第一张池图，冒烟不该把 12 张图 × 全波数都跑一遍
    # （全量是本地开发手动跑的活）。errors="replace"：runner 的 stderr 万一
    # 出现非 ASCII 字节（例如异常路径回显含中文的路径），别让 locale（GBK）
    # 解码把检查器自己打崩。
    proc = subprocess.run(
        [args.runner, "--maps", args.maps_dir, "--seeds", "1",
         "--max-waves", "2", "--max-ticks", "6000", "--limit", "1",
         "--out", args.out_json],
        capture_output=True, text=True, errors="replace", timeout=600)
    if proc.returncode != 0:
        print(f"runner 退出码非零: {proc.returncode}\nstderr:\n{proc.stderr}",
              file=sys.stderr)
        return 1

    try:
        data = json.loads(Path(args.out_json).read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 —— 报出来就够了，具体异常类型不重要
        print(f"输出不是可解析的 JSON: {exc}", file=sys.stderr)
        return 1

    assert data.get("schema") == "calibration_runner/1", data.get("schema")
    assert isinstance(data.get("maps"), list) and data["maps"], "maps 为空"
    runs = data.get("runs")
    assert isinstance(runs, list) and len(runs) == 1, f"runs 应有且仅有一局，实为 {len(runs) if isinstance(runs, list) else '非列表'}"
    run = runs[0]
    assert run["seed"] == 1, run.get("seed")
    assert "map_file" in run and run["map_file"].endswith(".json")
    for key in ("defeated", "truncated", "final_wave", "total_ticks"):
        assert key in run, f"run 缺字段 {key}"

    waves = run["waves"]
    assert len(waves) >= 1, "一局至少记下一波（--max-ticks 6000 足够越过首波开打）"
    w1 = waves[0]
    assert w1["wave"] == 1, w1.get("wave")
    for key in ("start_tick", "end_tick", "length_ticks", "build_ticks",
                "assault_start_tick", "nominal_level", "attacker_start",
                "attacker_end", "breach", "rams_spawned", "rams_died_en_route",
                "ram_events", "phoenix_events", "phoenix_died_tick",
                "volley_hits", "gate_windows", "defender_start",
                "defender_end", "defender_loss", "bld_start", "bld_end",
                "keep_hp_start", "keep_hp_end"):
        assert key in w1, f"wave 1 缺字段 {key}"
    assert w1["assault_start_tick"] > w1["start_tick"], "开打应晚于开波"
    assert w1["keep_hp_start"] > 0, "波首堡垒血量必须 > 0"
    assert w1["attacker_start"].get("Ghoul", 0) > 0, "第 1 波应有 Ghoul"
    assert isinstance(w1["volley_hits"], list)
    assert isinstance(w1["gate_windows"], list)
    for w in waves:
        assert w["end_tick"] >= w["start_tick"], w
        assert w["length_ticks"] == w["end_tick"] - w["start_tick"] + 1, w
        # 损失自洽：波首有而波末全灭的类型不会出现在 defender_end 里（键缺失 =
        # 0），loss 必须等于 波首 − 波末。这才是这条测试要钉的不变量。
        for k, v in w["defender_start"].items():
            end = w["defender_end"].get(k, 0)
            assert w["defender_loss"].get(k, 0) == v - end, (k, v, end)
    print(f"ok: 1 run, {len(waves)} wave(s), schema 与关键字段自洽")
    return 0


if __name__ == "__main__":
    sys.exit(main())
