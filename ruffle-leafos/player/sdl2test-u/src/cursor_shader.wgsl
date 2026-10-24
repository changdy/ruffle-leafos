// ============================================================
//  Cursor Shader — 任意宽高比 + 抗锯齿 + 实心/空心/描边
// ============================================================

struct CursorUniform {
    // xy: 鼠标在 NDC 空间的位置
    // z:   圆的半径（像素）
    // w:   描边宽度（像素），0 表示实心圆
    @align(16) pos_radius_stroke: vec4<f32>,
    // xy: 屏幕尺寸 (width, height) 像素
    // z:   抗锯齿宽度（像素）
    // w:   未用
    @align(16) screen_aa: vec4<f32>,
    // rgba 光标颜色
    @align(16) color: vec4<f32>,
};

@group(0) @binding(0)
var<uniform> u: CursorUniform;

// ---- 顶点输入 ----
struct VsIn {
    @location(0) pos: vec2<f32>,   // 单位圆 [-1, 1]
};

// ---- 顶点输出 → 片段着色器 ----
struct VsOut {
    @builtin(position) clip_pos: vec4<f32>,
    @location(0) local: vec2<f32>, // 局部坐标 [-1, 1]，用于距离计算
};

// ---- 顶点着色器 ----
@vertex
fn vs_main(input: VsIn) -> VsOut {
    let sw = u.screen_aa.x;
    let sh = u.screen_aa.y;
    let r  = u.pos_radius_stroke.z;

    // 像素半径 → NDC 半径，x/y 分别转换以补偿宽高比
    let r_ndc_x = (r / sw) * 2.0;
    let r_ndc_y = (r / sh) * 2.0;

    // 缩放单位圆 → 屏幕空间正圆
    let scaled = vec2<f32>(
        input.pos.x * r_ndc_x,
        input.pos.y * r_ndc_y,
    );

    // 偏移到鼠标 NDC 位置
    let ndc = u.pos_radius_stroke.xy + scaled;

    var out: VsOut;
    out.clip_pos = vec4<f32>(ndc, 0.0, 1.0);
    out.local    = input.pos;  // 保持 [-1, 1]
    return out;
}

// ---- 片段着色器 ----
@fragment
fn fs_main(input: VsOut) -> @location(0) vec4<f32> {
    let r        = u.pos_radius_stroke.z;
    let stroke   = u.pos_radius_stroke.w;
    let aa_px    = u.screen_aa.z;

    // local 坐标系中到中心的距离：中心=0, 边缘=1
    let dist = length(input.local);

    // 抗锯齿宽度（归一化到 [0,1] 范围）
    let aa = aa_px / r;

    // ===== 实心圆（stroke == 0） =====
    if stroke <= 0.0 {
        let alpha = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, dist);
        if alpha < 0.005 {
            discard;
        }
        return vec4<f32>(u.color.rgb, u.color.a * alpha);
    }

    // ===== 圆环 / 描边 =====
    let inner = 1.0 - (stroke / r);

    // 外边缘 alpha：从 outer(1.0) 向外淡出
    let outer_alpha = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, dist);

    // 内边缘 alpha：从 inner 向内淡出
    let inner_alpha = 1.0 - smoothstep(inner - aa, inner + aa, dist);

    // 圆环 = 外部可见 - 内部可见
    let ring_alpha = outer_alpha - inner_alpha;

    if ring_alpha < 0.005 {
        discard;
    }

    return vec4<f32>(u.color.rgb, u.color.a * ring_alpha);
}
