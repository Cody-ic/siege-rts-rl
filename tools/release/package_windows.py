"""Create an allowlisted player ZIP; never distribute the source workspace."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--font', type=Path, required=True)
    parser.add_argument('--font-license', type=Path, required=True)
    parser.add_argument('--crt', type=Path, required=True)
    parser.add_argument('--raylib-license', type=Path, required=True)
    parser.add_argument('--json-license', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', default='1.0.0')
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2]
    root=args.output/f'Sanctum-{args.version}-Windows-x64'
    root.mkdir(parents=True,exist_ok=False)
    data=root/'游戏数据'
    data.mkdir()
    def copy(src,dst):
        dst.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src,dst)
    binary=args.build/'render/Release/rts_render.exe'
    copy(args.launcher,root/'圣城.exe')
    copy(binary,data/'rts_render.exe')
    copy(args.font,data/'NotoSansSC.ttf')
    copy(args.font_license,data/'licenses/NotoSansSC-OFL.txt')
    copy(args.raylib_license,data/'licenses/raylib-LICENSE.txt')
    copy(args.json_license,data/'licenses/nlohmann-json-LICENSE.txt')
    for name in ('msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll',
                 'msvcp140_codecvt_ids.dll','vcruntime140.dll','vcruntime140_1.dll','concrt140.dll'):
        copy(args.crt/name,data/name)
    for src in sorted(binary.parent.glob('onnxruntime*.dll')):
        copy(src,data/src.name)
    for src in sorted(binary.parent.glob('ONNX-*.txt')):
        copy(src,data/'licenses'/src.name)
    for src in sorted((repo/'game/data/maps/pool').glob('*.json')):
        copy(src,data/'game/data/maps/pool'/src.name)
    copy(repo/'game/data/stats_placeholder.json',data/'game/data/stats_placeholder.json')
    # Keep only shipped sprite metadata and actual frames, not contact sheets or previews.
    for src in sorted((repo/'tools/sprite_gen/out_3d').iterdir()):
        if src.name=='_sprite_meta.json' or (src.suffix=='.png' and not src.name.startswith('_')):
            copy(src,data/'tools/sprite_gen/out_3d'/src.name)
    (root/'开始游玩.txt').write_text(f'''圣城 · v{args.version}

1. 右键下载的 ZIP，选择“全部解压”。
2. 双击“圣城.exe”，在菜单选择“开始新局”。

无需安装 Python、开发工具或额外模型。不要单独移动“圣城.exe”或“游戏数据”。
适用 Windows 10 / 11 64 位，需要支持 OpenGL 3.3 的显卡与驱动。

操作说明和图鉴都在游戏主菜单中。
B 建造，Tab 切换建筑；拖动建墙；Shift + 右键强制抢修。
空格暂停；F5 保存；J 阅读本局日记。每 10 波自动暂停展示剧情。
关闭后再次启动，可从主菜单继续已保存对局；新开局重置剧情。

存档与日志：%LOCALAPPDATA%\\SiegeRTS
旧版存档不兼容时会保留备份，请使用对应旧版继续旧局。
本发布版默认使用脚本攻方；未将实验 RL 权重设为默认。

如遇错误，请将“launcher.log”连同问题描述反馈给项目组。
''',encoding='utf-8-sig')
    manifest={'version':args.version,'source_commit':subprocess.check_output(
        ['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),'files':{}}
    for path in sorted(root.rglob('*')):
        if path.is_file():manifest['files'][path.relative_to(root).as_posix()]=digest(path)
    (data/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    archive=args.output/(root.name+'.zip')
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for path in sorted(root.rglob('*')):
            if path.is_file():z.write(path,path.relative_to(root.parent))
    checksum=args.output/'SHA256SUMS.txt'
    checksum.write_text(f'{digest(archive)}  {archive.name}\n',encoding='ascii')
    print(json.dumps({'archive':str(archive),'bytes':archive.stat().st_size,'sha256':digest(archive)},indent=2))


if __name__=='__main__':
    main()
