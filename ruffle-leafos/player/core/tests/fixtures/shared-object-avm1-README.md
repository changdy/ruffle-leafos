# AVM1 / AMF0 升级兼容夹具

这组测试与 AVM2/AMF3 回归独立，覆盖《拳皇》等 AVM1 游戏使用的 SharedObject 分支。`shared-object-avm1.swf` 是本项目原创动作流，源生成器为 `scripts/make_shared_object_avm1_fixture.py`；不依赖 ASC 或 playerglobal。它执行以下逻辑：

```actionscript
stop();
var progress = SharedObject.getLocal("leaf-shared-avm1", "/");
var savedData = progress.data;
// trace marker、Unicode text、counter、ratio、enabled、items.join 和 profile 字段。
savedData.marker = "migrated-avm1";
savedData.counter += 1;
savedData.items.push("new");
savedData.profile.level += 1;
savedData.profile.note = "persisted";
// 不调用脚本 flush；由真实 Player::flush_shared_objects_checked() 提交。
```

`shared-object-amf0-legacy.sol` 是纯合成数据，不含用户存档。它由旧 fork 锁定的 [flash-lso@998f47c926b9986aabd518fbb7394ff56936d0b0](https://github.com/ruffle-rs/rust-flash-lso/tree/998f47c926b9986aabd518fbb7394ff56936d0b0) 实际 **Amf0Writer/ObjWriter** 生成；与旧 `avm1/globals/shared_object.rs::new_lso()` 使用同一 writer API，包括 Number、String、Bool、ECMAArray 和嵌套 Object。没有使用新 writer 或 AMF3 改头伪装 AMF0，也没有声称执行过完整旧播放器。

固定电影 URL 为 `file:///compatibility-test.swf`，`localPath="/"`，旧 key 为 **`localhost//leaf-shared-avm1`**。原 fork 与新版 AVM1 getLocal 的 URL/localPath/key 生成源码块经字节规范化换行后比较完全一致。测试 backend 只在该旧 key 返回种子并断言实际读写 key，不从新 VM 反推 key 重新播种。

公开 PlayerBuilder/StorageBackend 集成测试应验证：旧字段经真实 VM 加载和 trace；脚本改写后 checked flush 成功，再用新 Player 重开读到更新；拒写时 checked flush 返回 false、旧字节保持，允许写入后重试和重开成功。与 AVM2 回归一样，这不能替代 RLS1 容器、SD 卡原子写/备份、实际游戏旧档或断电验收。

在工程根 `ruffle-leafos/` 重建：

```powershell
python scripts/make_shared_object_avm1_fixture.py --output ../work/ruffle-upgrade-071/avm1-storage-fixtures
```

旧 writer 使用独立 `work/ruffle-upgrade-071/old-lso-target`。只收录下表两个小测试输入，不收录生成的旧 writer 工程、Cargo.lock、编译缓存和重复校验输出。

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `shared-object-avm1.swf` | 825 B | `0299bb30c0723351dbe8fa3a205e53fcd01c7bb6ff54ee0f3df562dd223f1f3f` |
| `shared-object-amf0-legacy.sol` | 222 B | `0ba6370f18e057177bc0f64177f85d1539dd38976ed48b3a954b1748c8718898` |
