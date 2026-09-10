"""Export the approved Markdown to a self-contained HTML reading edition."""
from pathlib import Path
import argparse
import html

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs/holy-city/微笑者_副官陆衡小传.md"
TARGET = SOURCE.with_suffix(".html")
HEAD = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>微笑者 —— 副官陆衡小传</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; background: #0b0b14; color: #c5c5d3;
    font-family: "Noto Serif SC", "Source Han Serif SC", "Songti SC", "SimSun", serif;
    font-size: 18px; line-height: 1.95; }
  main { max-width: 680px; margin: auto; padding: 64px 36px 96px; }
  header { text-align: center; margin-bottom: 64px; }
  h1 { font-size: 36px; font-weight: 400; letter-spacing: .3em; color: #d6dbeb; }
  .subtitle { text-align: center; font-size: 15px; color: #a3aec4; }
  h2 { font-size: 22px; font-weight: 500; color: #b6c2da; margin: 56px 0 28px; }
  h3 { font-size: 17px; font-weight: 500; color: #b6c2da; margin: 40px 0 24px; }
  p { margin: 0 0 20px; overflow-wrap: break-word; }
  strong { font-weight: 700; }
  em { font-style: italic; }
  @media (max-width: 600px) {
    main { padding: 36px 24px 64px; } body { font-size: 17px; }
    h1 { font-size: 30px; } header { margin-bottom: 44px; }
  }
  @media print {
    :root { color-scheme: light; } body { background: white; color: black; }
    h1, h2, h3, .subtitle { color: black; } main { max-width: none; padding: 0; }
    h2, h3 { break-after: avoid; } p { orphans: 2; widows: 2; }
  }
</style>
</head>
<body>
<main>
"""


def render(source: str) -> str:
    blocks = source.strip().split("\n\n")
    assert blocks[:2] == ["# 微笑者", "## ——副官陆衡小传"]
    result = [HEAD, '<header><h1>微笑者</h1><p class="subtitle">——副官陆衡小传</p></header>']
    for block in blocks[2:]:
        assert block != "---", "The approved edition has no horizontal separators"
        if block.startswith("### "):
            result.append(f"<h3>{html.escape(block[4:])}</h3>")
        elif block.startswith("## "):
            result.append(f"<h2>{html.escape(block[3:])}</h2>")
        elif block.startswith("***") and block.endswith("***"):
            result.append(f"<p><strong><em>{html.escape(block[3:-3])}</em></strong></p>")
        else:
            result.append(f"<p>{html.escape(block)}</p>")
    return "\n".join(result) + "\n</main>\n</body>\n</html>\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check HTML matches Markdown without writing")
    args = parser.parse_args()
    rendered = render(SOURCE.read_text(encoding="utf-8-sig"))
    if args.check:
        if not TARGET.exists() or TARGET.read_text(encoding="utf-8") != rendered:
            raise SystemExit("HTML is stale; run python tools/export_smiler.py")
        print("HTML matches the approved Markdown")
    else:
        TARGET.write_text(rendered, encoding="utf-8")
        print(TARGET)
