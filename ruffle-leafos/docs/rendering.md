# 渲染与诊断

## 后端与显示链路

默认 Flash 后端为 **OpenGL ES**，实验 Vulkan 使用与核心同一基线的官方 wgpu 30。Pocket1 驱动缺少 `VK_KHR_wayland_surface`。前端由 GBM 分配 ABGR8888 缓冲，以 DMA-BUF 及明确的 modifier 和 plane layout 导入 Vulkan，在自定义 `RenderTarget` 绘制，再用 EGLImage/GLES 采样并叠菜单。此链路的常规帧通过共享图像呈现。[Vulkan 后端](../player/sdl2test-rocknix/src/vulkan_backend.rs) · [wgpu 来源锁](../vendor/ruffle-wgpu.lock.json)

队列提交由 Flash 线程串行执行。绘制完成后以 barrier 将图像释放到 `QUEUE_FAMILY_EXTERNAL` / GENERAL，有限时长的 fence wait 后交给 EGL/GLES。GLES 以 `glFinish` 完成采样后才开始下一帧。resize 和退出先等待 GPU，再释放同步对象和共享内存。Vulkan 使用 `MemoryHints::MemoryUsage`，控制分配器选取子分配块的策略。活动资源、独立大分配与驱动开销仍可能造成内存压力。

`leaf-graphics actual=` 标明实际 Flash 后端。`leaf-vulkan` 记录设备、布局与呈现。即使 Vulkan 负责 Flash 绘制，`GL_*` 仍来自 GLES 显示上下文。全局 Vulkan 初始化失败时提示并回退 GLES。运行期故障结束会话、尝试提交进度并恢复全局 GLES 偏好。显式 `--renderer opengl-es|vulkan` 只覆盖本次会话，故障时保留已保存的全局偏好。Apps 使用 GLES。[设置契约](formats/graphics-settings.md) · [设备验证与压力边界](validation/stable-device-20261102.md)

## 渲染资源与内存

| 资源 | 生命周期与预算 |
| --- | --- |
| 静态形状 | 首次实际绘制才上传，实例共享。缩放细分变体分别登记到弱引用活动队列。每个完整游戏帧渲染后收集，GLES 基于估算负载与最近呈现帧保留至多 16 MiB、1,024 个闲置形状。未知负载的后端在一帧闲置后释放。[静态回收](../player/core/src/static_render_resources.rs) |
| 不可变 CPU 位图 | 解码像素默认 8 MiB，按 `Vec.capacity()` 计费，最多 1,024 项。降零单次清空，卸载来源直接原地清理，保留外部持有的像素。非零预算按 LRU 淘汰。[位图缓存](../player/render/src/bitmap_cache.rs) · [清理基准](validation/cache-maintenance-20261107.md) |
| GLES 临时池 | 滤镜 scratch 纹理闲置上限 16 MiB / 32 项，读回 PBO 8 MiB / 16 项。scratch 按尺寸复用，PBO 优先取最小足够大的缓冲或复用后扩容。GL 线程释放超限对象。[GLES 后端](../player/sdl2test-rocknix/src/gl_backend.rs) |
| wgpu 资源 | 形状和帧命令保留 `BitmapHandle`，使弱缓存能复用活跃纹理。BitmapData 的自身 bind group 避免持有自身句柄。内存压力释放闲置纹理、Globals 和读回缓冲，继续允许在途请求完成。压力前借出的项迟到归还也会被淘汰。[wgpu 实现](../player/render/wgpu/) |

`RUFFLE_BITMAP_CACHE_MIB=0..16` 和 `RUFFLE_STATIC_CACHE_MIB=0..32` 可调留存预算，0 表示不留存。`RUFFLE_BITMAP_PREDECODE=1` 启用单线程后台预解码，默认关闭，仅处理不超过 1 MiB 的合格 Lossless 素材，最多四个排队、执行或待领取任务，输出计入像素预算。Flash 主线程不等待后台结果，仍可同步解码。位图解压初始容量按 Lossless/JPEG3 alpha 尺寸估算并封顶 4 MiB，解压结果可继续增长。[缓存实验](validation/resource-cache-20261030.md)

CachePolicy 每秒采样 `MemAvailable`，低于 128 MiB 或读取失败时将留存预算降为零，超过 160 MiB 时恢复。CPU 像素和闲置临时池立即退让，静态形状在下次完整游戏绘制后回收。预算只计估算的可留存资源，活动对象、在途绘制、驱动开销和游戏主动分配另计。

VulkanMemoryGuard 独立处理持续低内存。实际后端为 Vulkan，且有效读数持续低于 128 MiB 至少一秒时，停止会话并尝试提交原生进度。无效读数、读数恢复或实际后端为 GLES 时清除连续状态。采样点之间仍可能发生 OOM，保护触发和外部测试脚本中止分别记录。[Leaf 会话](../player/sdl2test-rocknix/src/leaf.rs) · [Pocket1 压力与拒写](validation/stable-device-20261102.md)

### 截图读回

截图使用当前舞台 `view_matrix()` 与 `movie_size()` 计算屏幕内裁剪，在暂停重绘后只读回一次 RGBA。PNG 按行转换 RGB、流式写盘。重绘前和读回前要求有效 `MemAvailable` 至少等于 RGBA 读取量加 2 MiB 编码器余量及 32 MiB 系统余量。截图保持当前缩放和可见内容。[编码实现](../player/sdl2test-rocknix/src/screenshot_png.rs) · [像素与拒写验证](validation/screenshots-20261031.md)

## 按需性能诊断

在已有 Leaf 启动环境中给播放器传入 `--trace-performance`，可配合 `--diagnostic-seconds`、`--renderer` 和独立 `--storage-dir`、`--controls-dir`。默认关闭追踪，开启后每秒输出统计。主线程处理任务轮询、VM tick、显示树遍历、GC、渲染与同步解码。CPAL/ALSA、Mali 驱动和物理 input 观察另有线程。[性能采样](../player/sdl2test-rocknix/src/performance.rs) · [核心计时](../player/core/src/render_metrics.rs)

| 日志或字段 | 读法 |
| --- | --- |
| `leaf-perf` | `frames` 是 Flash 帧推进量，`draws` 是非暂停绘制次数，分母用实际窗口时长。循环耗时排除主动休眠，比较时排除暂停与恢复边界窗口。 |
| `shape_ms`、`vk_submit_ms`、`vk_poll_ms`、`vk_present_ms` | 渲染内部墙钟计时，包含部分上传、fence、轮询及最终采样等待。已计入 `render_ms`，分别解读。 |
| `preload_ms`、`enter_ms`、`construct_ms`、`scripts_ms`、`exit_ms`、`audio_ms`、`mouse_ms`、`gc_ms` | VM 与宿主各阶段的计时。回调可能嵌套 goto 或其他阶段，不能直接求和，AVM1 脚本也不单独纳入 AVM2 Scripts。 |
| `leaf-gl-memory` | 上传、空目标、闲置 scratch/PBO、在途读回和模板缓冲按逻辑字节计数。RSS 与 Mali 页需另外采集。 |
| `pixel_*`、`predecode_*`、`static_idle_*`、`static_current_*` | 像素命中/淘汰、后台任务和静态资源。各字段峰值可能来自不同帧，任务与命中也可能跨窗口。 |

生成原创夹具可用 `python scripts/build_cache_diagnostic.py`（静态位图循环）、`python scripts/build_filter_cache_diagnostic.py`（单实例 Glow 尺寸变化）以及 `python scripts/make_bitmap_copy_fixture.py --output build/diagnostics/bitmap-copy`（BitmapData 像素对照，需要 JDK 和 playerglobal）。[工具准备](build-release.md#工具链与首次准备) · [拳皇内存采样](validation/kof-gles-memory-20261031.md) · [死神换回合读回](validation/bvn33-pve-stutter-20261031.md)
