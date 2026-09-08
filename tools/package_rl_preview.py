"""Build a self-contained Windows RL preview and verify every ZIP member."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def write_json(path,value):path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def package(build,attacker,defender,output,archive):
    build,attacker,defender,output,archive=map(Path,(build,attacker,defender,output,archive))
    if output.exists() or archive.exists():raise ValueError('Package outputs must be new')
    model=json.loads((defender/'manifest.json').read_text())
    if model['format']!='defender-macro-onnx-v2':raise ValueError('Native v2 model required')
    for name in ('encoder.onnx','decoder.onnx'):
        if sha(defender/name)!=model['graphs'][name]:raise ValueError('Defender graph hash differs')
    if sha(ROOT/'game/data/stats_placeholder.json')!=model['stats_sha256']:
        raise ValueError('Model statistics differ from packaged game')
    commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    dirty=subprocess.check_output(['git','diff','HEAD','--name-only','--','game','render','rts_core'],cwd=ROOT,text=True)
    if dirty.strip():raise ValueError('Commit runtime source changes before packaging')
    output.mkdir(parents=True)
    def copy(source,relative):
        target=output/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
    for name in ('rts_render.exe','onnxruntime.dll','ONNX-Runtime-LICENSE.txt','ONNX-Runtime-ThirdPartyNotices.txt'):
        copy(build/name,name)
    for source in sorted((ROOT/'game/data').rglob('*.json')):
        copy(source,source.relative_to(ROOT))
    for source in sorted((ROOT/'tools/sprite_gen/out_3d').iterdir()):
        if source.is_file() and source.suffix.lower() in ('.png','.json'):
            copy(source,source.relative_to(ROOT))
    copy(attacker,'models/attacker.onnx')
    for name in ('encoder.onnx','decoder.onnx','manifest.json'):
        copy(defender/name,Path('models/defender')/name)
    launchers={'play.bat':' --rl-policy "%~dp0models\\attacker.onnx"',
        'watch-ai.bat':' --rl-policy "%~dp0models\\attacker.onnx" --defender-policy "%~dp0models\\defender"',
        'play-script.bat':''}
    for name,flags in launchers.items():
        (output/name).write_text('@echo off\nchcp 65001 >nul\ncd /d "%~dp0"\nstart "" "%~dp0rts_render.exe"'+flags+'\n',encoding='utf-8',newline='\r\n')
    (output/'README.txt').write_text('圣城 RL 实验试玩包\n\n'
        'play.bat：玩家守城，攻方由现有战术模型控制。\n'
        'watch-ai.bat：攻守双方模型运行，可观察守方经营、建造和征兵。\n'
        'play-script.bat：普通脚本模式。\n\n'
        'HUD 的 Defender AI 表示守方已托管；攻方 RL 数字是实际受控编队数。\n'
        '不同模式与模型组合使用独立默认存档目录。继续旧局须保留原模型文件。\n'
        '守方候选在四个独立局面上的平均守住波数为 3.25，原模型为 2.75；\n'
        '这些是 Python 采样评估，原生游戏随机流不同，不能直接套用为游戏胜率。\n'
        '仅是小样本实验结果，尚未掌握 70 波；攻方宏观编成学习仍未完成。\n'
        '需要 Windows x64、中文字体和系统运行库。尚未在全新 Windows 机器验证。\n'
        '文件校验见 manifest.json，运行时与模型来源见 source.json。\n',encoding='utf-8')
    write_json(output/'source.json',dict(runtime_source_commit=commit,packager_sha256=sha(__file__),
        runtime_exe_sha256=sha(build/'rts_render.exe'),attacker_sha256=sha(attacker),
        defender_checkpoint_sha256=model['checkpoint_sha256'],defender_manifest_sha256=sha(defender/'manifest.json')))
    manifest={p.relative_to(output).as_posix():sha(p) for p in sorted(output.rglob('*')) if p.is_file()}
    write_json(output/'manifest.json',manifest)
    archive.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(archive,'x',compression=zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(output.rglob('*')):
            if path.is_file():bundle.write(path,path.relative_to(output).as_posix())
    with zipfile.ZipFile(archive) as bundle:
        if set(bundle.namelist())!=set(manifest)|{'manifest.json'}:raise ValueError('ZIP file set differs')
        for name,digest in manifest.items():
            if hashlib.sha256(bundle.read(name)).hexdigest()!=digest:raise ValueError('ZIP content differs: '+name)
    report=dict(archive=str(archive.resolve()),sha256=sha(archive),bytes=archive.stat().st_size,
                verified_files=len(manifest),runtime_commit=commit)
    print(json.dumps(report));return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','attacker','defender','output','archive'):parser.add_argument('--'+name,required=True)
    args=parser.parse_args();package(args.build,args.attacker,args.defender,args.output,args.archive)
