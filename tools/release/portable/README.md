# v1.0.1-Portable 单文件封装工具

本工具封装现有 v1.0.1 玩家发布包，不重新编译或改变游戏规则。
只需分发一个 EXE；启动时校验内嵌 ZIP，释放至当前用户的独立工作目录，再运行原版启动器。
原启动器等待游戏退出后，封装程序删除本次释放的目录。
存档和日志继续保存在 `%LOCALAPPDATA%/SiegeRTS`，不随临时资源删除。
程序异常终止可能留下 `SiegeRTS/portable/run-*` 临时目录，退出所有游戏后可手动清理。

## 当前完成与未完成

2026-09-17 已在 Windows x64 编译并完成单文件启动验收。生成 EXE 通过独立中文空格路径的
素材校验、菜单截图、脚本→RL→脚本往返、1800 tick 战斗、退出清理及用户数据校验文件保留测试。
实际记录见 `VALIDATION.txt`。未重新进行真人完整游玩、F5 保存后交互式继续对局或全新系统测试。
源码目录不提交游戏 ZIP 与生成 EXE；玩家应从 GitHub Release 下载单文件 EXE。

## Windows 构建

在 Windows 10/11 x64 上完整解压本工具包，打开 PowerShell：

```powershell
.\build-portable.ps1 -InputZip .\Sanctum-1.0.1-Windows-x64.zip
```

若单独使用仓库里的脚本，可省略 InputZip，脚本会下载官方 ZIP。
使用 Windows 的 .NET Framework C# 编译器，不需要重新配置 C++、raylib 或训练环境。
脚本未设置或绕过 PowerShell 执行策略；若系统策略禁止脚本，按所在环境允许的构建方式执行。
生成文件：

- `dist/Sanctum-1.0.1-Portable-Windows-x64.exe`
- `dist/SHA256SUMS.txt`

构建脚本固定输入包 SHA256，拒绝不一致输入，也拒绝覆盖已有输出。
包装器依赖 Windows 的 .NET Framework 4.5+；标准 Windows 10/11 包含兼容版本。
这是新增封装层的系统组件依赖，不能描述成“零依赖”。游戏原有的 OpenGL 3.3 要求保持不变。
.NET 官方依据：https://learn.microsoft.com/en-us/dotnet/framework/install/versions-and-dependencies

## 构建后验收

```powershell
.\verify-portable.ps1 -Executable .\dist\Sanctum-1.0.1-Portable-Windows-x64.exe
```

该脚本把唯一 EXE 复制到新的中文空格路径，隔离 LOCALAPPDATA，执行素材、菜单、RL 往返和战斗检查，
检查退出码、游戏日志及临时资源清理。保留验收目录与日志作为证据。
它不替代实际游戏验收。在没有仓库和开发环境的 Windows 电脑上，仅复制生成的 EXE 后再检查：

1. 断网后双击 EXE，主菜单正常打开，字体、素材和音效正常。
2. 开始新局，建设、征兵和进入战斗正常。
3. F5 保存，完全退出，再次双击同一 EXE 能继续原局。
4. 脚本切换 RL 再切回脚本，分别恢复各自对局。
5. 正常退出后没有本次 run-* 临时资源，存档仍存在。
6. 运行无需管理员权限，无需另行安装 Python、训练框架或游戏运行库。

重复启动会分别解压，不复用可能损坏的缓存。不同运行仍沿用原版存档路径，
不要同时运行多局写入同一个存档。
参数会传给原版启动器；如传入截图或自定义存档路径，应使用绝对路径，避免写进临时资源目录。

## 发布

全部 Windows 验收通过后，使用有发布权限的账号创建 Release：

- Tag：`v1.0.1-Portable`
- 标题：`圣城 v1.0.1-Portable · 单文件启动版`
- 正文：参考 `RELEASE_NOTES.md`，补入实际验收环境和 EXE SHA256。
- 附件：生成的 EXE 和 SHA256SUMS.txt。
- 保留现有 v1.0.1 ZIP 作为备用，不覆盖已有附件。

建议把本目录先提交到仓库，再将 Portable 标签指向包含包装器源码的提交。
游戏本体仍取自 fb0c36d6f121d24521a618c801393f17ad71f748，发布说明中区分两者。
Portable 是额外分发形式；命名中的连字符按严格语义化版本规则属于预发布标识，
但 GitHub 是否标记为 Pre-release 由发布设置单独决定。验收通过前只保留为草稿。
