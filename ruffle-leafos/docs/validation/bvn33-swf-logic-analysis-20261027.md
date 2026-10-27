# 死神 VS 火影 3.3：SWF 脚本优化候选分析

日期：2026-10-27。用户明确希望分析游戏 SWF 内的逻辑，并选择稍后再做实机测试。本轮只做静态分析，没有修改 SWF、播放器或设备安装，也没有产生新的实机性能结论。

## 分析对象和证据边界

分析现有离线版主 SWF，SHA-256 为 `148592ba6fbffc79938660997ff8ae6d38c4f90d962208c46c41626199a9a4f5`。脚本来自前次 JPEXS 反编译结果。本轮重新核对原游戏与离线版的四个 DoABC 标签：仅第 4 个标签包含前次已记录的 19 字节启动 NOP，其余字节一致。因此下列游戏方法未在离线适配中改写。

反编译来源位于本地 `outputs/bvn33-diagnosis-20261026/full-scripts/scripts/`。本轮检查记录与各来源 SHA-256 位于 `outputs/bvn33-swf-logic-20261027/evidence.json`，不提交版权游戏及其完整反编译源码。

## 优先候选一：计时器停止重复重建数字

调用链为 `GameCtrl.render → GameState.render → FightUI.render → FightBar.render → FightTimeUI.render`。

- `FightTimeUI.as:53` 每次刷新都将当前整数秒数赋给 `_numMc.number`，没有检查显示值是否已相同。
- `MCNumber.as:47–59` 的 setter 每次都删除旧数字、清空数组并重新创建数字 MovieClip；`createNum` 又在第 69 行调用 `gotoAndStop`。
- 对两位计时器，即使这一秒内数字没有变化，也会反复拆掉并重建两个显示对象。它还会产生新的时间轴跳帧、显示树变更和分配回收工作。

优先修改 `FightTimeUI.render` 这一调用点：只有当前整数秒数与 `_numMc.number` 不同时才赋值。构造函数已经完成首次显示，因此可保留首次初始化；倒计时的计算、递减频率、结束判定、位数和数字素材不变。先限定在计时器调用点，避免直接为所有 MCNumber setter 加缓存而影响其他控件的样式或主动重建行为。

预计主要消除相同数字的重复重建。实际节省多少时间、能否解释练习模式与实战的部分差异，必须用同一播放器和场景的 SWF 对照验证，当前不能给出 FPS 改善幅度。

## 优先候选二：气槽档位不变时省去重复跳帧

`FightUI.as:166–167` 每次刷新两个 QiBar；`QiBar.render` 调用内部 `InsBar.setProcess`。该方法在 `QiBar.as:339` 无条件对气槽数字调用 `gotoAndStop(int(process) + 1)`。气量可以连续变化，而用于显示的整数档位通常保持不变。

候选改法：保持气量插值、条宽、可见性和援助进度更新，仅当数字 MovieClip 的目标帧不同于当前帧时跳帧；还需保持其停止状态。这样不会减少气量的计算和实际档位切换。

前轮播放器采样表明，嵌套 goto 会涉及全局帧构建及帧脚本遍历，故这一点值得优先实验。但省去同帧 goto 会减少相应的帧生命周期事件，不能未经验证宣称与原版完全等价。主游戏反编译脚本未检出 frameConstructed/exitFrame 监听；外部资源 SWF 的相关脚本尚未全面核查。需要检查数字素材的帧脚本、停止状态，以及气量达到或跌破 1/2/3 档、放必杀消耗后恢复、训练和多局重新绑定等路径。

## 次级候选和已排除的重复建议

`GameMainLogicCtrler.checkBodyHit` 在方法开头创建捕获外部变量的 `getVec` 闭包，之后才执行开关、类型、穿透和矩形相交等提前返回检查。可评估将闭包创建推迟到实际需要位移计算时，或改成等价辅助方法，减少不碰撞对象对的临时分配。它涉及碰撞位移和重量比例，优先级低于两处 UI 重复工作；需要保持原有公式与执行顺序，目前没有证据认定它是主要耗时。

`FighterMC` 已按当前动画帧缓存身体和攻击区域，不能笼统建议“新增碰撞框缓存”。`KyoClassLoader` 也已保存 ApplicationDomain 并调用 unloadAndStop，不能把“卸载资源根对象”当作尚未实现的游戏优化。角色的 nextFrame、goFrame 和动画子节点遍历会触发动作及命中帧，本轮不提出直接跳过它们。

## 后续验证范围

先制作两个可单独启用的实验副本，分别只改计时器和气槽档位更新，保留现有离线版与全部资源。核对修改的 ABC 方法范围，并反编译复核；同一已安装播放器对照静止练习场和单人实战，测量 tick、构建/帧脚本耗时、数字对象创建和跳帧次数。再检查倒计时结束、气槽切档/消耗、技能、暂停恢复和多局。没有这些验证前，不替换正式游戏，不声称稳定 30/60 帧。

设备旧地址连接超时，用户已选择暂不测试；这不影响上述脚本层候选的定位。此前提交的显示树优化属于播放器层，不能视作本轮已经完成了 SWF 逻辑优化。

## 打斗卡顿的进一步脚本候选

用户进一步要求关注实际打斗。本节仍为静态分析，未修改游戏或重新实测；前述 UI 更新也在战斗主循环中执行，并非只影响菜单。

### 动态残影和滤镜位图

`ShadowEffectView.addShadowBp`（约第 96 行）先计算角色 bounds，再调用 `KyoUtils.drawDisplay`；后者再次读取尺寸/bounds，创建 BitmapData 并 draw。残影消失时又 dispose。`BitmapFilterView.renderBitmapData`（约第 88 行）在帧计数变化时销毁旧位图；`KyoUtils.drawBitmapFilter` 创建源和滤镜结果两份 BitmapData，执行 draw/applyFilter 后再销毁源位图。

这是冲刺、残影和滤镜类招式期间值得测量的路径。可先复用同一次捕获已计算的 bounds，再评估同尺寸位图缓冲的有限复用，保持捕获频率、滤镜和视觉效果。活动残影必须各自保留独立快照，缓冲只能在旧残影彻底结束后回收复用；还需清空旧像素、处理尺寸变化并限制缓存字节数。不能用同一可写缓冲覆盖多个仍可见的残影，也不能仅按主 MovieClip.currentFrame 判断画面不变，因为子动画仍可能推进。

普通 EffectView 已复用 EffectVO.bitmapDataCache，EffectManager 也已有闲置 EffectView 对象池；EffectModel.initlize 已执行 cacheEffects。故不把“给所有打击特效新增对象池/首次预热”当作新发现，候选限定于上述动态截图和滤镜缓冲。

### 碰撞计算中的临时矩形

`FighterAILogicBase.targetInRange` 仅为了获得布尔结果调用 intersection(...).isEmpty()，可以评估改用 intersects。已核对当前播放器 `flash/geom/Rectangle.as:140–168`：两者采用相同的空矩形与轴对齐重叠计算，而 intersection 会额外构造结果 Rectangle。

`GameMainLogicCtrler.getHitObj`（约第 369 行）对每个候选攻击框创建交集矩形，无相交时也分配。可评估先做布尔相交判断，仅对确实命中的框生成传给 beHit 的 hitRect；命中时会重复部分数学计算，是否值得取决于实战中的命中/未命中比例。原命中顺序、边缘接触和空矩形规则必须保持，不减少判定次数或扩大命中范围。

### AI 距离及目标列表的短寿命分配

`FighterAILogicBase.getTargetDistance` 每次创建 Point，部分调用点只读取 x；这些调用可直接进行标量运算，保留决策频率与随机调用顺序。`FighterMain.getTargets` 为每个目标队伍调用 getAliveChildren 生成 Vector，再 concat 生成新 Vector；攻击实体和援助的目标检查也会走这条路径。可评估按原顺序一次性填充结果，减少中间 Vector，避免跨帧缓存导致死亡、援助、分身或召唤物列表过期。

不能把 getCurrentHits/getBodyArea 的结果简单整帧复用：现有命中对象的 currentArea 会更新，碰撞位移及 beHit/hit 回调也可能在同一轮遍历中改变对象状态。需要先记录调用次数和分配量，再决定更复杂的缓存是否值得。

### 主动打击停顿与低帧率应分开记录

`EffectModel` 为多种命中效果配置 freeze，例如 50 ms 和 400 ms。`EffectCtrl.freeze` 将时间换算为 FPS_GAME 帧数并暂停战斗逻辑，renderFreeze 每次更新减一后恢复。这是游戏主动的打击停顿，可能与真正的低帧率叠加；若停顿期间自身未达到目标帧率，按帧倒数也可能拉长实际墙钟持续时间，但目前没有对此做实测。

后续应分别记录命中/招式、freeze 状态、实际帧间隔、残影和滤镜启用情况。缩短或取消 freeze 属于改变打击表现与时序，不能作为不改玩法的性能提升。角色 nextFrame、goFrame、AI 更新频率及命中帧也不直接删减。

测试优先按现象选择：冲刺/残影/特定滤镜期间的尖峰先查动态位图；持续连招慢速先统计碰撞和列表分配，同时保留计时器/气槽两个低风险实验。没有新增性能数据前，不承诺哪一项贡献最大或能达到多少 FPS。
