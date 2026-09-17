# 圣城 v1.0.1-Portable · 单文件启动版

下载 `Sanctum-1.0.1-Portable-Windows-x64.exe`，双击启动。
无需手动解压或把“游戏数据”文件夹放在 EXE 旁边。

本版本封装 v1.0.1 的完整官方发布包，游戏规则、内置模型及 v6 存档格式保持不变。
启动时自动释放资源，正常退出后清理。首次画面出现前需要等待资源释放。
它是单文件分发，不是所有资源永久仅存在于内存的程序。

运行条件：Windows 10/11 x64，系统 .NET Framework 4.5+，支持 OpenGL 3.3 的显卡和驱动。
无需管理员权限，无需安装 Python、编译工具或单独下载 RL 模型。
标准 Windows 10/11 包含兼容的 .NET Framework；精简系统需要先确认该组件可用。

存档沿用 `%LOCALAPPDATA%/SiegeRTS`。原版 ZIP 仍可作为备用下载。
日志：`launcher.log`（游戏）及 `portable.log`（单文件包装器）。

## 来源与校验

- 游戏源码：`fb0c36d6f121d24521a618c801393f17ad71f748`
- 内嵌包：`Sanctum-1.0.1-Windows-x64.zip`
- 内嵌包 SHA256：`2de9442252d3f5f741f2f002f4e376ae926c07451a4c8186782716356e07510a`
- 包装器源码：本标签所指提交中的 `tools/release/portable`。
- Portable EXE SHA256：`b36b1d09b9361900553ed07afe0ad9711e8f37ef75456fd732a99e3417d399db`。
- 实际验收：2026-09-17，Windows x64（build 26200.9457），.NET Framework 4.8.09221。
  单个 EXE 在工程外中文空格路径通过素材、菜单、脚本↔RL、1800 tick 战斗和退出清理检查。
  用户数据校验文件保持原样；这不等价于新增真人保存/读档验收。
- 未进行全新系统测试、真人完整游玩和音效验收；程序未做数字签名。游戏载荷与 v1.0.1 完全一致。
