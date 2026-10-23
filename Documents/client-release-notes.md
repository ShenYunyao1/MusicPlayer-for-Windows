# MusicPlayer2 独立发行 0.1.0

基于上游 MusicPlayer2 2.78，支持 MusicServer `/api/v2`。本期兼容基线为服务器 0.16.2 / schema 7。

- 使用账户所属 API Key 访问全局曲库、个人歌单、公开订阅及指定账户只读分享。
- 支持分页与增量刷新、按需下载、内容校验、离线缓存，以及授权撤回和无效订阅状态。
- 修复 HTTPS 协议判断错误导致的连接测试 HTTP 400。
- 独立 Actions 验证、构建与发布 Windows x64 白名单包，附提交号、SHA-256 和发行清单。
- 新增参数化 publish.bat／publish.ps1：检查工作区与版本后推送源码或正式版本标签，跟踪 Actions，并核验 Release 资产；支持预览和仅触发模式。
- Windows 产物经 Ubuntu 再次校验；修复打包成功返回码与默认配置换行差异导致的误判。

下载 `MusicPlayer2.zip`，完整解压后启动其中的 `MusicPlayer2/MusicPlayer2.exe`。不要只复制 EXE；目标电脑需要 Microsoft Visual C++ x64 运行库。服务器地址填写根地址，API Key 由使用者自行配置。包内不包含个人账号、配置、歌单、缓存或测试数据。

这是独立客户端发行号，上游程序版本与服务器版本另行标识。SHA-256 用于校验下载完整性，本期不提供 Authenticode 签名或客户端一键自动更新。x86 仅做编译检查，暂不分发。
