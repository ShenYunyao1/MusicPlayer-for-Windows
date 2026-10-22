# 播放器运行依赖输入

本目录集中保管原仓库已有的第三方运行库，输出目录中的副本可通过构建重新生成。现有头文件和导入库保留在 `MusicPlayer2/`，第三方来源与许可沿用原项目。

- `common/Encoder/`：已有外部编码器可执行文件，作为子进程使用。
- `Win32/common/`：原 `Debug/` 的 BASS/BASS FX、原源码目录的 Win32 插件和编码 DLL。
- `Win32/Debug/`、`Win32/Release/`：原相应配置的 `tag.dll`。
- `x64/common/`：原 `x64/Debug/` 的 BASS/BASS FX，与原 x64 Release 中的副本哈希一致。
- `x64/Debug/`、`x64/Release/`：原相应配置的 `tag.dll`。
- `defaults/`：首次构建使用的便携模式配置；已有运行配置不会被覆盖。

原 x64 Release 目录中的 Plugins 和 Encoder DLL 实际均为 Win32，与源码内的 32 位依赖相同。这些 DLL 仅复制到 Win32 输出，不放进 x64 输出冒充 64 位插件。本次不升级或下载第三方库；x64 的可选解码/编码 DLL 需要另行补齐，BASS 主内核、BASS FX 和 tag.dll 已按架构保留。

SciLexer.dll 由仓库内 Scintilla 源码构建，不在这里保存旧编译副本。播放器程序、中间文件、个人配置及媒体位于 Git 忽略的 `build/`；测试输出位于 `test-results/`，白名单分发包位于 `dist/`，均不放入本目录。
