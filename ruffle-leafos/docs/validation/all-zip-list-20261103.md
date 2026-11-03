# 三款游戏 ZIP 与 Leaf 索引验证（2026-11-03）

Miniloong Pocket 1 / Leaf 0.13.0；设备正式安装为 0.8.3。本轮安装包将拳皇单 SWF 也打入 ZIP，死神 3.3 / 3.6 ZIP 保留原资源布局。打包测试 5 项通过，构建安装包使用播放器 `f4dc303617ac116e29f8f61824b685986980db2d16add3427fc13c24e4e4c233`。

临时备份正式 pak 清单，仅将 Flash 的 ZIP 设为 pass-through；将三个测试 ZIP 放入单独的 Flash 子目录，触发 Leaf 实际扫描。generation 87 无扫描错误，数据库中三个 ZIP 各有一条 FLASH 游戏记录，归档内资源没有被索引为游戏。本次查询的是 Leaf 索引，未拍摄列表页面，且设备旧目录仍在，因此不代表升级后页面已验收。

拳皇 ZIP 使用同一候选播放器和隔离数据目录直接启动：日志显示 ZIP 解压、主 SWF 加载、OpenGL ES 上下文创建和 MOVIE STARTED，持续输出渲染帧。该次未进行战斗验收；两款死神的先前隔离启动结果见 [ZIP 加载验证](zip-loading-20261102.md)。Leaf IPC 使用正确参数后接受启动请求，并解析到 FLASH 和 ruffle_probe；但本次未观察到测试启动脚本执行，因此未验证从 Leaf 游戏列表选择 ZIP 到启动播放器的完整链路，也未正式安装新版。

测试后恢复正式清单和启动脚本并核对哈希，将测试 ZIP 移到 Roms 之外，Wi-Fi 省电恢复为 on。最后 generation 90 扫描完成且无错误，测试游戏索引为空，SQLite integrity_check 为 ok。未覆盖正式游戏、配置或存档；测试文件与运行日志保留在设备隔离目录和本地 outputs。
