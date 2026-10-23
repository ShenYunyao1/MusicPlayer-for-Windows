# Windows 播放器 Actions 构建、测试与独立发布

日期：2026-10-24。适用于独立仓库 MusicPlayer2，服务器仓库继续独立维护。

## 目标与边界

源码推送 → 自动化验证 → 固定名称构建产物；推送匹配版本的 `vX.Y.Z` 标签 → 所有门禁通过 → 草稿上传完整资产 → 公开正式 Release。开发阶段本地针对性检查并提交，未经本次明确授权不推送、不创建远程仓库、不发布。

当前客户端 `origin` 指向原作者 `zhongyang219/MusicPlayer2`，本地功能尚未同步至用户的独立仓库。首次推送前需要用户提供自己的目标仓库；原作者仓库应保留为 upstream，不向其推送。此阶段不修改远程地址。

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

普通分支不公开 Release；PR 无写权限；测试失败不能发布；版本标签必须匹配本地清单；发布包与接受测试的同一构建一致；固定资产 `MusicPlayer2.zip`、`MusicPlayer2.zip.sha256`、`client-release-manifest.json` 不含个人配置、Key、数据库和测试数据。

首次远程验收仍需后续明确推送授权及目标仓库。推送后关注 Actions 成功状态与正式 Release 资产，再勾选远程验收。

- [ ] 用户目标仓库已确定，首次推送及 Actions 全部通过。
- [ ] 首次标签发行资产与提交号核对通过。

## 本地验收记录

2026-10-24：actionlint 1.7.7、工作流 YAML 与发布权限／依赖检查通过；Python 发布门禁测试通过；当前实际 ZIP 的白名单、架构、默认配置、SHA-256、提交号及发行身份检查通过，现有打包污染／缺失／错误架构回归也通过。真实服务集成通过首次改密、分页、媒体校验、下架／恢复、停用／启用、订阅失效／恢复、指定账户接受／撤回／重邀，以及服务中断后的离线恢复。

构建环境依据 [Windows 2022 运行器清单](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)，发布权限与作业依赖依据 [GitHub Actions 工作流语法](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)。运行时仍检查 VS 2022 MFC 是否存在，缺失时明确失败。

未推送、未触发远程 Actions、未发布 Release，未连接或变更云服务器。交互桌面播放不属于本次工作流调整的本地验收范围。
