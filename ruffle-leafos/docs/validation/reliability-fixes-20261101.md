# 输入、保存提示与构建绑定验证（2026-11-01）

基线：`ef6d61f`。三项改动按问题分别提交，相关验证使用合并候选；不表示每个中间提交单独完成完整构建/实机矩阵。正式应用未替换，版本保持 0.8.3 开发预览。

## 候选与主机结果

ARM64 release ELF SHA-256：`bbb8243a6d7871f2e3a1dbfdbc5427e27f41affe3a3d3162281133c9c2d608b4`。Rust 1.94.0，generic、opt-level 2、无跨 crate LTO，实际最高 GLIBC 2.35。

前端状态使用 `python -X utf8 scripts/test_session.py` 在 Windows 实际执行，143 项通过，含 4 项新增轴/中立门回归和 2 项保存状态回归。UTF-8 选项仅作用于该次 Python 进程，避免主机默认 GBK 错读测试输出。

原始主机报告、候选元数据、设备日志/截图及备份保留于本地忽略目录 `outputs/reliability-fixes-20261101/`，不提交设备数据副本。

## 1 有效轴归中与恢复

`Mapping::used_stick_axes()` 按 X/Y 轴上是否有实际绑定判断是否参与归中；方向互换后使用交换后的实际动作。`Frontend` 使用与派发相同的 active/legacy/config snapshot 刷新中立门缓存，恢复、换方案及相关模式变化同步更新。
未使用轴清零后再走原有 0.18 径向死区，摇杆派发使用同一过滤，避免闲轴噪声改变活跃轴的归一化结果。物理按键和活跃偏移轴仍需松开/归中；软件派生的 `Source::Stick` 由轴快照判定，避免等待派生 key-up 才能恢复的循环阻塞。

新增主机回归覆盖未绑定轴偏移、单轴噪声、None/Custom/方向互换、派生方向滞留及真实按键保护。既有 `--self-test-slots` 增加实际 Front 断言，实机结果见后续合并验证摘要。本项不识别坏掉的活跃轴，也不提供可调死区/曲线/滞回。

## 2 构建源码输入与 ELF 绑定

共享 `scripts/build_inputs.py` 直接遍历实际文件内容，不依赖 Git HEAD、暂存区、mtime 或来源锁标签。保守覆盖 player workspace 的 Rust/AS3/Cargo/.cargo、ASC、shader/字体/嵌入资源，target-libs 库/pc/来源清单，以及实际构建/链接脚本和工具锁。未提交新增、删除及同大小/mtime 变化都进入确定性相对路径→SHA 清单。
Markdown 文档、来源元数据 leaf-source.lock.json、运行游戏/安装资源和已知生成/本机目录不属于 ELF 编译输入。实际 pkg-config 绝对路径字节保留，因此这是本工作区的过期产物检查，不是跨机器可复现构建或供应链签名；外部 Cargo 依赖与工具仍沿用既有锁和版本记录。

build 开始即作废旧成功记录；生成本机包装器/pc 后、Cargo 前取输入快照，编译后再取一次。输入变化、编译或 ELF 检查失败不覆盖旧 ELF，也不留下可打包成功记录；成功才将输入清单/摘要和 ELF SHA 写入 player-build.json。播放器用 pending 文件替换，避免半写入。
打包拒绝旧 schema/缺少或损坏快照、ELF 不符、当前输入变化；复制播放器后核对暂存字节，并在 ZIP 前复核。包含源码时还核对源码暂存树本身，防止复制过程中变更后新锁给旧 ELF 改标签。

新增 25 项 Python 回归：24 通过、1 因 Windows 无符号链接创建权限跳过；既有 4 项打包和 15 项发布脚本回归通过。自动 Release 已在 ARM64 构建前接入同一套绑定回归，本轮未新增线上 runner 执行验收。实际重建后的 1,463 项输入/ELF 绑定核验通过；安装/源码 ZIP 结果另见后续合并摘要。

## 3 保存失败提示与成功重试

`Ui.save_error` 独立于 `message` 和 3 秒 toast，只有实际 `flush_progress()` 的成功结果清除。请求重试、切页、修改其他设置和切换语言都不确认保存成功。失败不再写 page-local message，避免成功重试后旧错误再次出现；无关页面操作结果不被清空。
暂停主页显示明确重试路径，游戏存档页保留最近失败原因，当前槽 Y 使用既有保存入口。状态跨页面保留，横条只在暂停主页/存档列表显示，避免遮挡按键编辑、设置和其他页面的自身结果；退出、换槽、换后端的原保存失败保护保持。

两项主机回归核对跨页、重试请求与实际成功的区别、原因更新、中英文即时切换和无关消息保留。原创 `ProgressRetryProbe.as` 通过真实 SharedObject 产生初始有效档和后续待保存变化；`make_progress_retry_fixture.py` 在忽略目录生成 SWF/ABC，设备故障验证另见后续摘要。

## 合并候选的实际验证

| Pocket1 / Leaf 0.13.0 场景 | 实际结果 |
| --- | --- |
| GLES、实际 Vulkan，各一组完整输入自测 | 新增 neutral_axes、既有 independent_axes 和 slots self_test 均通过，正常退出并 flush；覆盖禁用轴偏移、单轴噪声、活跃轴保护、同帧恢复、方向互换和 legacy fallback |
| 中文 GLES、英文 Vulkan，各一组原生进度故障注入 | 初始有效档后，在本 case 的备份目标建立空目录阻止 rename；主档哈希保持，Home/Saves 跨页和失败 Y 重试后仍有提示；去故障后同页 Y 成功，旧提示清除，正常菜单保存退出 |
| 8 张提示截图 | 全部在 3.5 秒 toast 过期后目视核对；Home 重试路径、Saves 原因、失败重试保留、成功后清除均正确 |
| 原生数据检查 | 严格解析 RLS 校验和与 AMF3，marker 从 initial 变为 changed-after-input；两个后端的 .bak 分别与各自初始有效档完全同哈希 |

保存夹具前两轮未计为通过：默认 SharedObject localPath 包含测试目录 `.userdata`，被既有路径规则拒绝后返回 null。保留原 SWF/日志及诊断，修正夹具显式 localPath `/` 后在新 case 重测，未为测试更改播放器规则。
实际成功夹具 SHA-256：`609209a82c6f845ad763eaca57af44652c62b0c45e0f6883479b919f1102d326`。存储仍由独立 case 目录及 SWF 内容身份隔离。

正式应用、全部 Flash ROM、配置与 SharedObject 前后逐文件哈希一致。临时 Apps 入口已移至 Apps 外，备份数据库后刷新并核验 apps/apps_fts 无残留、其他 Apps 记录完全一致；没有手动删除旧索引记录。同 boot 内核无新增 OOM，无测试播放器残留。
原始证据见本地 `slots-summary.json`、`retry-gles-3-summary.json`、`retry-vulkan-summary.json`、`visual-review.json`、`cleanup.json` 及对应日志/截图。

## 实际打包绑定核验

`python -X utf8 scripts/package_preview.py --output ../outputs/reliability-fixes-20261101/package --include-source` 通过。
源码输入摘要：`2ec694df7b704b99df4e5e52276cf511317b54ac9b10bacc9bd8d7c77fd05884`；源码 ZIP 内逐一复核全部 1,463 项输入字节 SHA，安装 ZIP 内播放器 SHA 与构建记录/清单一致，两份 ZIP CRC 检查通过。

- 安装 ZIP SHA-256：`a702f5ef6926fe90a530d5d7a936af065001c0d24b9ecc74f8dac9f0a03ba7b4`。
- 源码 ZIP SHA-256：`4ad1c8ffa1e555ab1962e678d6a15f0ea02a3339cdda8db037673b69ea6d7f8f`。

这些是忽略输出目录中的验证产物，未发布、未安装到正式入口。报告/文档非 ELF 输入，因此本报告完成后的文字整理不要求重新编译；绑定范围不承诺整个工作区的任意资源都参与 ELF。

## 验证边界

没有新增真实满卡、只读介质、拔卡、断电、多进程并发保存、广泛实战或长时验收。此次故障注入针对备份目标无法替换；不把这一种 IO 失败扩大为全部存储故障。
活跃故障轴的自动识别、可调死区/曲线/滞回、完整保存失败取消/明确不保存退出对话及历史备份/导入导出仍待实现；本轮没有改变现有数据格式或宣称性能提速。
