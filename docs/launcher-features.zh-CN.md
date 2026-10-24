# 账号、游戏目录与下载源

## 离线账号

在“设置 → 账号”中选择“Add Offline”，输入玩家名即可。不需要先登录微软账号，也不会进入试玩模式。普通启动可联网补全游戏文件；“离线启动”使用已经下载的文件。离线身份不能用于要求正版认证的服务器。

## LittleSkin 与其他皮肤站

在“设置 → 账号”选择“添加皮肤站 / LittleSkin”。默认 API 地址为：

```
https://littleskin.cn/api/yggdrasil/
```

输入皮肤站邮箱或用户名与密码。启用二步验证时请使用站点支持的应用密码。存在多个角色时会显示角色选择框。也可以填写其他兼容 authlib-injector/Yggdrasil 的 HTTPS API 地址。

密码只用于本次登录，不写入账号文件；客户端令牌、访问令牌与角色信息按账号配置保存。令牌失效后会要求重新登录。启动游戏时自动下载 authlib-injector，并校验官方发布元数据中的 SHA-256；校验失败会阻止启动。已校验的缓存可复用，但每次使用前仍检查文件哈希。账号认证不经过游戏下载镜像。注入器需要 Java 8 或以上版本。皮肤与服务器会话验证由注入器接入该皮肤站；启动器现有的微软皮肤编辑器只适用于微软账号。

## 默认目录

全新安装默认在启动器可执行文件旁的 `.minecraft` 保存数据：

```
启动器目录/
  prismlauncher
  .minecraft/
    versions/        # 各版本/实例
    libraries/       # 共享运行库
    assets/          # 共享资源
    meta/            # 启动器版本元数据
    authlib-injector/
```

目录必须可写；安装在受保护的系统目录时，可使用 `--dir <可写目录>` 指定数据根目录。已有标准数据目录的配置、环境变量指定目录、`--dir` 和便携目录标记继续有效，不会自动迁移旧数据。旧的 `instances` 文件夹存在时仍作为默认实例位置。此布局继续使用 Prism 的实例与组件元数据；不会自动把 HMCL/原版的版本 JSON 转换为 Prism 实例。

## 版本隔离

全局“Minecraft”设置和单实例“设置”中新增“游戏目录与版本隔离”。

- 共享游戏目录：游戏工作目录为数据根目录。
- 所有版本隔离：工作目录为各自的版本目录。
- 仅模组版本隔离：启用 Forge、NeoForge、Fabric、Quilt 等加载器的实例使用独立目录，普通版本共享目录。
- 单实例还可选择跟随全局、自定义目录或保留旧实例目录。

独立目录隔离 `mods`、配置、存档、资源包等；运行库与资源缓存仍共享。旧实例已有的 `minecraft` / `.minecraft` 嵌套目录默认保留，避免更换目录后找不到存档。自定义相对路径相对于实例目录解析。修改隔离选项不会搬移文件；需要迁移时请先备份并手动复制对应的游戏数据。

## 下载源

在“设置 → Services”中的“游戏下载源”选择官方源、BMCLAPI 或自定义兼容镜像。自定义地址必须为 HTTPS，可包含路径前缀，不允许用户名、密码、查询参数和片段。

可勾选“镜像失败时回退至官方源”。镜像网络或 HTTP 错误时，下载器仅回退一次，继续使用原有文件校验规则。路由覆盖游戏版本文件、资源、Minecraft 运行库和 Forge/NeoForge Maven 依赖。启动器组件元数据保留原有 Meta URL 设置；Modrinth、CurseForge 等平台及账号认证使用各自的地址。

## 验证

已加入回归测试，覆盖离线身份启动模式、账号持久化、目录解析与旧布局、下载源路由、镜像失败回退及校验、Yggdrasil 认证与刷新、非法响应拒绝，以及注入器下载与缓存哈希校验。

构建后执行：

```
ctest --test-dir <构建目录> --output-on-failure --no-tests=error
```

真实 LittleSkin 账号登录及游戏中的皮肤渲染需要可访问站点的网络、用户账号和实际游戏环境。本地协议测试不等同于这些真实账号及游戏内验证。

本次环境验证还读取了 LittleSkin 的真实 API 元数据，下载并校验了官方 authlib-injector 1.2.8，并以 Java 17 完成注入器初始化。云环境中的 JVM 需要显式使用平台提供的 HTTPS 代理路由；代理主机与端口可通过 Java 的 `https.proxyHost`、`https.proxyPort` 参数配置，不能假定 JVM 自动读取 `HTTPS_PROXY`。未关闭 TLS 校验。

## 多平台 CI 与 Release

每次分支推送会运行 Linux 验证及多平台 Release 配置构建。多平台目标包括 Linux x64/ARM64、Windows MSVC x64/ARM64、Windows MinGW 和 macOS 通用包。各目标先执行测试，再生成 AppImage、便携包、Windows 安装器和 macOS ZIP/DMG。全部成功后，工作流汇总 18 个发布文件（含源码包），附加 `SHA256SUMS.txt`，并上传 `release-assets-snapshot-<commit>` 工作流附件；普通分支推送不会公开创建 Release。

正式发布：将代码合并或选定提交后，推送形如 `v12.1.0` 的版本标签。标签触发流程全部通过后，会上传全部文件并自动公开 Release。`v12.1.0-beta.1` 这类带后缀的标签自动标记为预发布，不替换最新稳定版。

也可手动运行 **Multi-platform packages and release** 工作流：默认仅构建和上传附件；勾选 `publish` 后必须填写指向所选提交的现有版本标签。工作流拒绝标签与构建提交不一致的情况，以及覆盖已经公开的 Release。失败的发布可保留草稿，修复后重跑继续上传。

发布只需要仓库提供的 `GITHUB_TOKEN`；可选 Windows/macOS/Linux 签名凭据沿用原有配置，不配置时会生成未签名或临时签名包。macOS 公证也需要独立配置。仓库可选变量 `MSA_CLIENT_ID` 用于自行注册的微软登录客户端；不会在发行包中使用上游项目的微软、CurseForge 或 Imgur 凭据，也不启用上游 Sparkle 更新源。此分支不会向上游 PrismLauncher 的 Winget 标识发布。
