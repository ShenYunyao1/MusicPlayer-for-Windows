MusicPlayer2 x64 便携版

解压整个 ZIP，双击 MusicPlayer2.exe。请将 DLL、language、skins 和 Encoder 与程序一起保留。
目标电脑需要 Microsoft Visual C++ x64 运行库；若提示缺少 MFC140U.dll、MSVCP140.dll 或 VCRUNTIME140.dll，请安装后重试。
Microsoft 官方安装说明：https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist

首次运行会在程序目录保存个人配置，请解压到当前用户可写的位置。
连接音乐服务器：先在管理网页登录、完成首次改密并创建账号所属的 API Key；授予 library:read、media:read 并选择本人歌单或系统 library。
在播放器服务器设置中填写服务器根地址和 Key，再刷新服务器歌单。
Key 使用当前 Windows 用户的 DPAPI 加密保存。本分发包不包含开发者的账号、Key、歌单或媒体缓存。

BUILD-INFO.txt 记录此程序的编译时间、Git 提交号和 SHA-256。
源码许可见 LICENSE。现有 x64 可选解码/编码插件未补齐；本包包含 BASS 主内核、BASS FX、tag、SciLexer 和外部编码器。
