# SharedObject 升级兼容夹具

`shared-object-compat.swf` 是本项目原创 AS3 程序，源码位于 `scripts/resources/SharedObjectCompatProbe.as`。它通过公开 `SharedObject.getLocal("leaf-shared-compat", "/")` 读取进度、输出固定 trace 并修改数据；不自行 flush。主机核心回归调用真实 `Player::flush_shared_objects_checked()`，随后用新的 Player 重新打开。

`shared-object-legacy.sol` 是纯合成测试数据，不含用户存档。生成时实际运行旧 fork 锁定的 [flash-lso@998f47c926b9986aabd518fbb7394ff56936d0b0](https://github.com/ruffle-rs/rust-flash-lso/tree/998f47c926b9986aabd518fbb7394ff56936d0b0) 写入器，使用旧 `core/src/avm2/globals/flash/net/shared_object.rs::new_lso()` 相同的 `Lso::new(..., AMFVersion::AMF3)` 封装。它包含字符串、Unicode、整数、Double、Bool、混合 Array 和嵌套 Object。没有声称用旧完整播放器运行过此夹具；旧 writer 与新版真实 VM 的交叉验证有意分开，避免读写都由新版本生成而掩盖迁移问题。

读取测试固定电影 URL 为 `file:///compatibility-test.swf`。旧 fork 和 v0.7.1 的 getLocal 存储 key 规则经逐行核对保持一致：file 来源使用 `localhost`，`localPath="/"` 去掉首尾斜杠后为空，最终旧 key 为 **`localhost//leaf-shared-compat`**。测试 backend 只在该 key 返回旧字节，并断言实际 get/put key；不会从新实现计算 key 再重新播种。

两个回归分别验证旧数据读取/改写/重开，以及拒绝写入时 checked flush 返回 false、原字节保持、允许写入后重试成功且重开读到更新。它们运行实际 VM、AMF 解码/编码和 StorageBackend 接口；不替代 Leaf RLS1 容器、真实 SD 卡原子写/备份、断电或升级安装验收。

在 `ruffle-leafos/` 下使用已准备的工具链与 playerglobal 重建：

```powershell
python scripts/make_shared_object_fixture.py --output ../work/ruffle-upgrade-071/shared-object-fixtures
```

生成器将 ASC/ABC、中间源文件、独立旧 writer Cargo 工程保留在忽略目录；旧 writer 使用 `work/ruffle-upgrade-071/old-lso-target`，不修改播放器 Cargo.lock。只将最终小型 SWF/SOL 作为版本化测试输入放入本目录。测试由 `python scripts/test_core.py` 执行。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `shared-object-compat.swf` | 1,136 B | `73ddc8fd248d61dc32c3a6b31085b051781a4b8d3707ffb623dcb906ee0d4647` |
| `shared-object-legacy.sol` | 169 B | `1dd4238ffc904168be84f54fc83cfaf8591c3fcea5cd70a4a6ba33aaceaeeb5b` |
