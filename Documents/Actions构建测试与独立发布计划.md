# Windows 播放器 Actions 构建、测试与独立发布

日期：2026-10-24。适用于独立仓库 MusicPlayer2，服务器仓库继续独立维护。

## 目标与边界

源码推送 → 自动化验证 → 固定名称构建产物；推送匹配版本的 `vX.Y.Z` 标签 → 所有门禁通过 → 草稿上传完整资产 → 公开正式 Release。开发阶段本地针对性检查并提交，未经本次明确授权不推送、不创建远程仓库、不发布。

独立公开仓库已按用户要求命名为 [ShenYunyao1/MusicPlayer-for-Windows](https://github.com/ShenYunyao1/MusicPlayer-for-Windows)，本地 `origin` 已指向该仓库；原作者 `zhongyang219/MusicPlayer2` 保留为 `upstream`，`remote.pushDefault` 为 `origin`。仓库创建阶段未推送任何代码；首次源码推送及 Actions 验收在下方单独记录。

当前服务器与客户端名称均为暂定名称。用户确定正式名称后，两端在同一命名调整阶段统一更新仓库、界面标识、文档与链接，并检查 Actions、发行包、部署及更新器引用，避免只改其中一端。当前不调整服务器仓库名或固定产物名。

本期正式分发沿用已验证的 Windows x64 白名单 ZIP。保留 x86 编译检查，x86 分发需另行完善其依赖与打包验收，不发布旧式零散 EXE/DLL 包。发行版本由 `client-release.json` 独立维护，从 0.1.0 开始；上游程序版本 2.78 和服务器协议适配标识分别保留，发行身份以 ZIP 中 BUILD-INFO 与清单为准。

本期不是播放器自动更新器：用户下载、完整解压并启动新版。ZIP 和 SHA-256 不等同于 Windows Authenticode 签名，不复用服务器更新器的签名密钥。

## 实施清单

- [x] 用 `.github/workflows/windows-release.yml` 替换旧 `ci.yaml`：master/main 推送、PR 和手动触发；固定 Windows 2022 / VS 2022；最小权限、超时、日志产物；标签发布依赖全部验证成功。actionlint 与 YAML 门禁检查通过，远程执行待验收。
- [x] 接入现有布局、原生 HTTP/TLS、分页／缓存、白名单打包回归；真实服务器集成使用固定提交的隔离测试服务，不访问云端实例。已核对命令及真实服务测试结果，完整 CI 仍待执行。
- [x] 修复真实服务测试的外部源码位置和旧版号硬编码，显式校验所选服务器版本与 schema。本地针对 0.16.2 / schema 7 的临时实例通过。
- [x] 增加 `client-release.json`、发行说明及可重复的版本／ZIP／SHA-256／提交号校验脚本；错标签、错误清单或包污染必须失败。发布门禁单元测试及当前实际 ZIP 校验通过。
- [x] 记录本地验收与提交范围；未推送阶段明确保留“Actions 待执行”，不把静态验证计为远程运行成功。

## 验证范围

本地仅运行新增发布检查的有效／失败用例、语法／工作流检查和受影响的真实服务集成；沿用已通过的客户端 x64 构建，不重复完整桌面播放回归。Actions 执行完整的非交互测试门禁。依赖交互桌面与音频设备的播放回归仍按功能变更进行人工／本地专门验收，不将其放到普通托管运行器冒充可靠门禁。

真实服务器测试依赖 Python pywinpty、Go 和 ffprobe。运行器分别 checkout 到同级 `player/` 与 `server-source/`，客户端 checkout 内的产物仍使用 `build/`、`test-results/`、`dist/`。这使客户端本身可以独立 checkout、构建和发布。未来升级兼容基线时同时更新服务器提交、版本和 schema，并由 CI 重新验证。

## 完成标准

普通分支不公开 Release；PR 无写权限；测试失败不能发布；版本标签必须匹配本地清单；发布包与接受测试的同一构建一致；固定资产 `MusicPlayer2.zip`、`MusicPlayer2.zip.sha256`、`client-release-manifest.json` 不含个人配置、Key、数据库和测试数据。Windows 上传的产物还须经 Ubuntu 作业重新验证，通过后才允许正式发布。

首次远程验收仍需后续明确推送授权。推送后关注 Actions 成功状态与正式 Release 资产，再勾选远程验收。

- [x] 独立公开仓库已创建，本地 origin／upstream 与默认推送目标已核对。
- [x] 首次源码推送完成，master 已跟踪 origin/master，未推送版本标签。
- [x] 首次 Actions 全部通过；Windows 编译、协议／缓存／集成、打包及 Ubuntu 产物复核均通过。
- [x] 首次标签发行资产与提交号核对通过：v0.1.0，三个固定资产完整，发布及下载复核通过。

## 本地验收记录

2026-10-24：actionlint 1.7.7、工作流 YAML 与发布权限／依赖检查通过；Python 发布门禁测试通过；当前实际 ZIP 的白名单、架构、默认配置、SHA-256、提交号及发行身份检查通过，现有打包污染／缺失／错误架构回归也通过。真实服务集成通过首次改密、分页、媒体校验、下架／恢复、停用／启用、订阅失效／恢复、指定账户接受／撤回／重邀，以及服务中断后的离线恢复。

构建环境依据 [Windows 2022 运行器清单](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)，发布权限与作业依赖依据 [GitHub Actions 工作流语法](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)。运行时仍检查 VS 2022 MFC 是否存在，缺失时明确失败。

未推送、未触发远程 Actions、未发布 Release，未连接或变更云服务器。交互桌面播放不属于本次工作流调整的本地验收范围。

2026-10-24：用户授权创建新仓库后，通过已登录的 ShenYunyao1 账号创建公开空仓库，GitHub 返回 ADMIN 权限、isPrivate=false、isEmpty=true。本地原 origin 重命名为 upstream，新 origin 为用户仓库；默认推送目标为 origin。仅核对仓库信息与 Git 远程配置，没有推送源码或标签。

2026-10-24：用户授权应用 Actions 并推送源码，随后要求调整仓库名称；仓库更名为 `MusicPlayer-for-Windows`，本地 origin 同步更改，upstream 保留。首次推送发现客户端是浅克隆，GitHub 拒收缺失历史对象的包；补全 upstream 历史后重试，不使用强制推送，不推送版本标签。

2026-10-24：提交 `0160e120e00b7aea5bcf78764db5390c76fb07e0` 已成功推送至 origin/master，GitHub 默认分支为 master，Actions 权限 enabled=true。远程已存在 `.github/workflows/windows-release.yml`，但首次导入后的工作流注册及运行列表暂为空；仍在核对首次 Actions，尚未标记远程测试通过或正式发行。

2026-10-24：工作流已注册为 active，首次 push 运行 `39998738941` 已启动；为验证最新提交，显式触发 `39998912814` 并取消较早的重复运行。最新运行的 x64／x86 编译、布局、发布门禁、协议／TLS／缓存、真实服务器集成和白名单打包回归全部通过，最终打包步骤误判失败：package.ps1 输出 PACKAGE SUCCEEDED，但没有设置成功返回码，干净 PowerShell 会话的 LASTEXITCODE 为 null，调用方将其判为非零。已先添加干净调用方回归并复现同样失败，再补齐成功时 exit 0；修复后的远程整体验收仍待重新运行。

2026-10-24：提交 b599a3fb 的自动 push 运行 `40000189910` 已成功，test、build-x86 均 success，release 按普通分支规则 skipped。下载构建产物进一步复核时发现默认配置在 Windows checkout 为 CRLF、本地为 LF；规范化换行后文本完全相同，但原始字节比较误判为个人配置，这也会阻断 Linux 发布校验。新增回归先复现该错误，再将默认文本校验调整为仅允许 CRLF/LF 差异，其他内容仍须严格相同；增加 Ubuntu verify-artifact 门禁，后续所有分支构建均检查 Windows 分发包在 Linux 上的校验结果。

2026-10-24：提交 `4e86bac4f191bed520d489c0589d2b047c4b2614` 的自动 push 运行 [40001277626](https://github.com/ShenYunyao1/MusicPlayer-for-Windows/actions/runs/40001277626) 全部通过：test、build-x86、verify-artifact 均 success，release 按普通分支规则 skipped。已下载 MusicPlayer2-windows-x64 产物，独立核对 ZIP 白名单、默认配置、PE 架构、大小／SHA-256、发行清单及 EXE 内嵌提交号，均与该提交一致。没有推送版本标签，Release 列表为空，云端服务器未变更。本条验收记录单独本地提交，不为文档更新重复触发整套构建。

2026-10-24：用户随后授权测试完成后在线发布。[参数化推送发布脚本](参数化推送发布脚本.md) 已完成隔离测试，并实际用于推送提交 `4f479f35f0005e84a4c016d06eff4614f59ca3fe` 和 v0.1.0 标签；标签运行 [40005476050](https://github.com/ShenYunyao1/MusicPlayer-for-Windows/actions/runs/40005476050) 的全部门禁与 release 均成功。[v0.1.0](https://github.com/ShenYunyao1/MusicPlayer-for-Windows/releases/tag/v0.1.0) 已公开为 latest，三个固定资产的大小／SHA-256、清单、ZIP 与 EXE 内嵌提交号均通过独立核对。服务器未部署或重启。
