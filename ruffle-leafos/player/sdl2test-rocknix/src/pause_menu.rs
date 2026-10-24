//! Small GL overlay. All GL state changed by the menu is restored before the
//! movie renderer runs again; changing labels use a bounded texture cache.
use ab_glyph::{point, Font, FontArc, PxScale, ScaleFont};
use glow::HasContext;
use std::path::Path;
use std::sync::Arc;

use crate::session::{MouseSpeed, Session};

struct Text {
    texture: glow::Texture,
    width: i32,
    height: i32,
}

pub struct PauseMenu {
    gl: Arc<glow::Context>,
    program: glow::Program,
    vertices: glow::Buffer,
    vao: glow::VertexArray,
    color: Option<glow::UniformLocation>,
    textured: Option<glow::UniformLocation>,
    texts: Vec<Text>,
    font: FontArc,
    live: Vec<(String, Text)>,
    palette: [[f32;4];6],
    pill_shape: u8,
}

impl PauseMenu {
    pub unsafe fn new(gl: Arc<glow::Context>, font_path: &Path, previews: &[[String; 4]]) -> Result<Self, String> {
        let font = FontArc::try_from_vec(std::fs::read(font_path).map_err(|e| format!("menu font: {e}"))?)
            .map_err(|_| "menu font is not a supported TrueType font".to_string())?;
        let program = gl.create_program()?;
        for (kind, source) in [
            (glow::VERTEX_SHADER, "#version 300 es\nprecision highp float;\nin vec2 position; in vec2 texcoord; out vec2 uv; void main(){uv=texcoord;gl_Position=vec4(position,0.0,1.0);}"),
            (glow::FRAGMENT_SHADER, "#version 300 es\nprecision mediump float;\nin vec2 uv; uniform sampler2D atlas; uniform vec4 color; uniform int textured; out vec4 pixel; void main(){float a=textured==1?texture(atlas,uv).r:1.0;pixel=vec4(color.rgb,color.a*a);}")
        ] {
            let shader = gl.create_shader(kind)?;
            gl.shader_source(shader, source);
            gl.compile_shader(shader);
            if !gl.get_shader_compile_status(shader) {
                let error = gl.get_shader_info_log(shader);
                gl.delete_shader(shader);
                gl.delete_program(program);
                return Err(format!("pause menu shader: {error}"));
            }
            gl.attach_shader(program, shader);
            gl.delete_shader(shader);
        }
        gl.bind_attrib_location(program, 0, "position");
        gl.bind_attrib_location(program, 1, "texcoord");
        gl.link_program(program);
        if !gl.get_program_link_status(program) {
            let error = gl.get_program_info_log(program);
            gl.delete_program(program);
            return Err(format!("pause menu link: {error}"));
        }
        let old_vao = gl.get_parameter_vertex_array(glow::VERTEX_ARRAY_BINDING);
        let old_buffer = gl.get_parameter_buffer(glow::ARRAY_BUFFER_BINDING);
        let vertices = gl.create_buffer()?;
        let vao = gl.create_vertex_array()?;
        gl.bind_vertex_array(Some(vao));
        gl.bind_buffer(glow::ARRAY_BUFFER, Some(vertices));
        gl.enable_vertex_attrib_array(0);
        gl.enable_vertex_attrib_array(1);
        gl.vertex_attrib_pointer_f32(0, 2, glow::FLOAT, false, 16, 0);
        gl.vertex_attrib_pointer_f32(1, 2, glow::FLOAT, false, 16, 8);
        gl.bind_vertex_array(old_vao);
        gl.bind_buffer(glow::ARRAY_BUFFER, old_buffer);
        let mut texts = Vec::new();
        for (label, pixels) in [
            ("暂停", 40.0), ("继续游戏", 32.0),
            ("通用控制：混合", 32.0), ("通用控制：键盘", 32.0),
            ("通用控制：鼠标", 32.0), ("返回 Leaf", 32.0),
            ("上下选择  左右预览  A 应用  B 返回", 23.0),
            ("混合：十字键和按钮操作游戏，摇杆移动鼠标，L 左键 R 右键", 20.0),
            ("键盘：使用游戏按键配置；无自定义时 A 空格 B 回车", 20.0),
            ("鼠标：摇杆或十字键移动，A 左键 B 右键，可按住拖拽", 20.0),
            ("鼠标速度：慢   Y 光标外观", 29.0), ("鼠标速度：标准   Y 光标外观", 29.0), ("鼠标速度：快   Y 光标外观", 29.0),
            ("仅调节摇杆和十字键鼠标；重开恢复标准。A 应用，B 取消预览", 20.0),
        ] {
            texts.push(Self::text(&gl, &font, label, pixels)?);
        }
        for name in std::iter::once("通用（沿用 cfg）").chain(crate::session::profiles::PROFILES.iter().map(|p| p.name)) {
            texts.push(Self::text(&gl, &font, &format!("按键方案：{name}"), 29.0)?);
        }
        texts.push(Self::text(&gl, &font, "预览尚未应用；A 应用，B 返回并取消未应用预览", 20.0)?); // 27
        texts.push(Self::text(&gl, &font, "左键保持：M+十字下解除；暂停时释放", 21.0)?); // 28
        for lines in previews {
            for line in lines { texts.push(Self::text(&gl, &font, line, 21.0)?); } // 29+
        }
        texts.push(Self::text(&gl, &font, "自定义按键", 30.0)?);
        Ok(Self {
            color: gl.get_uniform_location(program, "color"),
            textured: gl.get_uniform_location(program, "textured"),
            gl, program, vertices, vao, texts, font, live: Vec::new(),
            palette: Self::palette(),
            pill_shape: std::env::var("RUFFLE_MENU_PILL").ok().and_then(|s|s.parse().ok()).filter(|n|*n<4).unwrap_or(3),
        })
    }

    unsafe fn text(gl: &glow::Context, font: &FontArc, label: &str, size: f32) -> Result<Text, String> {
        let scaled = font.as_scaled(PxScale::from(size));
        let width = label.chars().map(|c| scaled.h_advance(font.glyph_id(c))).sum::<f32>().ceil() as i32 + 4;
        let height = (scaled.ascent() - scaled.descent()).ceil() as i32 + 4;
        let mut alpha = vec![0u8; (width * height) as usize];
        let mut x = 2.0;
        for c in label.chars() {
            let glyph = font.glyph_id(c).with_scale_and_position(size, point(x, scaled.ascent() + 2.0));
            if let Some(outline) = font.outline_glyph(glyph) {
                let bounds = outline.px_bounds();
                outline.draw(|gx, gy, coverage| {
                    let px = gx as i32 + bounds.min.x as i32;
                    let py = gy as i32 + bounds.min.y as i32;
                    if px >= 0 && py >= 0 && px < width && py < height {
                        let target = &mut alpha[(py * width + px) as usize];
                        *target = (*target).max((coverage * 255.0).round() as u8);
                    }
                });
            }
            x += scaled.h_advance(font.glyph_id(c));
        }
        let previous = gl.get_parameter_texture(glow::TEXTURE_BINDING_2D);
        let alignment = gl.get_parameter_i32(glow::UNPACK_ALIGNMENT);
        let unpack_buffer = gl.get_parameter_buffer(glow::PIXEL_UNPACK_BUFFER_BINDING);
        let unpack_params = [glow::UNPACK_ROW_LENGTH,glow::UNPACK_SKIP_PIXELS,glow::UNPACK_SKIP_ROWS];
        let unpack_values = unpack_params.map(|p| gl.get_parameter_i32(p));
        let texture = gl.create_texture()?;
        gl.bind_buffer(glow::PIXEL_UNPACK_BUFFER,None);
        for p in unpack_params { gl.pixel_store_i32(p,0); }
        gl.bind_texture(glow::TEXTURE_2D, Some(texture));
        gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
        gl.tex_image_2d(glow::TEXTURE_2D, 0, glow::R8 as i32, width, height, 0, glow::RED, glow::UNSIGNED_BYTE, Some(&alpha));
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR as i32);
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::CLAMP_TO_EDGE as i32);
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::CLAMP_TO_EDGE as i32);
        gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, alignment);
        for (p,v) in unpack_params.into_iter().zip(unpack_values) { gl.pixel_store_i32(p,v); }
        gl.bind_buffer(glow::PIXEL_UNPACK_BUFFER,unpack_buffer);
        gl.bind_texture(glow::TEXTURE_2D, previous);
        Ok(Text { texture, width, height })
    }

    unsafe fn quad(&self, rect: (f32, f32, f32, f32), color: [f32; 4], text: Option<usize>) {
        let (x, y, width, height) = rect;
        let x0 = x / 480.0 - 1.0;
        let x1 = (x + width) / 480.0 - 1.0;
        let y0 = 1.0 - y / 360.0;
        let y1 = 1.0 - (y + height) / 360.0;
        let data: [f32; 24] = [x0,y0,0.,0., x1,y0,1.,0., x0,y1,0.,1., x0,y1,0.,1., x1,y0,1.,0., x1,y1,1.,1.];
        self.gl.buffer_data_u8_slice(glow::ARRAY_BUFFER, bytemuck::cast_slice(&data), glow::STREAM_DRAW);
        self.gl.uniform_4_f32(self.color.as_ref(), color[0], color[1], color[2], color[3]);
        self.gl.uniform_1_i32(self.textured.as_ref(), i32::from(text.is_some()));
        if let Some(index) = text {
            self.gl.bind_texture(glow::TEXTURE_2D, Some(self.texts[index].texture));
        }
        self.gl.draw_arrays(glow::TRIANGLES, 0, 6);
    }

    unsafe fn label(&self, index: usize, x: f32, y: f32, color: [f32; 4]) {
        let t = &self.texts[index];
        self.quad((x, y, t.width as f32, t.height as f32), color, Some(index));
    }

    pub unsafe fn draw(&mut self, width: u32, height: u32, state: &Session, cursor_position: Option<(f32,f32)>) {
        let gl_handle = self.gl.clone();
        let gl = &gl_handle;
        let program = gl.get_parameter_program(glow::CURRENT_PROGRAM);
        let vao = gl.get_parameter_vertex_array(glow::VERTEX_ARRAY_BINDING);
        let buffer = gl.get_parameter_buffer(glow::ARRAY_BUFFER_BINDING);
        let draw_framebuffer = gl.get_parameter_framebuffer(glow::DRAW_FRAMEBUFFER_BINDING);
        let read_framebuffer = gl.get_parameter_framebuffer(glow::READ_FRAMEBUFFER_BINDING);
        let mut viewport = [0; 4];
        gl.get_parameter_i32_slice(glow::VIEWPORT, &mut viewport);
        let active_texture = gl.get_parameter_i32(glow::ACTIVE_TEXTURE) as u32;
        gl.active_texture(glow::TEXTURE0);
        let texture = gl.get_parameter_texture(glow::TEXTURE_BINDING_2D);
        let sampler = gl.get_parameter_sampler(glow::SAMPLER_BINDING);
        let capabilities = [glow::BLEND, glow::DEPTH_TEST, glow::STENCIL_TEST, glow::SCISSOR_TEST, glow::CULL_FACE];
        let enabled = capabilities.map(|c| gl.is_enabled(c));
        let mask = gl.get_parameter_bool_array::<4>(glow::COLOR_WRITEMASK);
        let blend = [glow::BLEND_SRC_RGB, glow::BLEND_DST_RGB, glow::BLEND_SRC_ALPHA, glow::BLEND_DST_ALPHA,
                     glow::BLEND_EQUATION_RGB, glow::BLEND_EQUATION_ALPHA].map(|p| gl.get_parameter_i32(p) as u32);
        gl.bind_framebuffer(glow::FRAMEBUFFER, None);
        gl.viewport(0, 0, width as i32, height as i32);
        gl.bind_vertex_array(Some(self.vao));
        gl.bind_buffer(glow::ARRAY_BUFFER, Some(self.vertices));
        gl.use_program(Some(self.program));
        gl.bind_sampler(0, None);
        gl.uniform_1_i32(gl.get_uniform_location(self.program, "atlas").as_ref(), 0);
        for cap in capabilities { gl.disable(cap); }
        gl.enable(glow::BLEND);
        gl.blend_func(glow::SRC_ALPHA, glow::ONE_MINUS_SRC_ALPHA);
        gl.blend_equation(glow::FUNC_ADD);
        gl.color_mask(true, true, true, true);
        if state.paused {
        if !state.experience.started {self.quad((0.,0.,960.,720.),self.palette[0],None);}
        if state.experience.page!=crate::session::experience::Page::Home {
            self.draw_experience(state);
        } else if state.controls.visible {
            self.draw_controls(state);
        } else {
        self.leaf_background(&state.experience.title,if state.experience.started{"Flash · 已暂停"}else{"Flash"},"继续游戏");
        let slot=state.controls.preview_slot;
        let rows=["继续","按键方案","编辑当前映射","鼠标速度","鼠标光标","保存游戏进度","游戏存档","画面与声音","保存并退出"];
        let details=[String::new(),format!("方案 {}  ◀ ▶",slot+1),String::new(),
            format!("{} · Y 光标",match state.preview_mouse_speed {MouseSpeed::Slow=>"慢",MouseSpeed::Normal=>"标准",MouseSpeed::Fast=>"快"}),
            String::new(),"原生进度".into(),format!("存档 {} / 5",state.experience.active+1),String::new(),String::new()];
        for (i,line) in rows.iter().enumerate() {
            self.leaf_row(i+1,line,138.+i as f32*51.,48.,i==state.selected);
            if !details[i].is_empty(){self.live_label_width(30+i,&details[i],31.,600.,145.+i as f32*51.,315.,false,self.palette[if i==state.selected{3}else{4}]);}
        }
        self.live_label_width(16,"M+A 上一方案 · M+B 下一方案 · M+十字下 左键保持",20.,23.,606.,910.,false,self.palette[4]);
        }
        } else if state.drag_locked {
            self.live_chip(61,"左键保持 · M+十字下解除",22.,(10.,if state.experience.settings.fps{48.}else{10.}),600.,false,[1.,0.8,0.4,1.]);
        }
        if let Some((message,_))=&state.toast {
                self.live_chip(60,message,25.,(480.,658.),920.,true,[0.96,0.99,1.,1.]);
        }
        if !state.paused {
            if state.experience.settings.fps {
                self.live_chip(62,&format!("FPS {:.1}",state.experience.fps),24.,(10.,10.),180.,false,[1.,1.,1.,1.]);
            }
            if let Some((x,y)) = cursor_position { self.draw_cursor(state.cursor_settings,(x*960./width.max(1) as f32,y*720./height.max(1) as f32)); }
        }

        for (cap, was_enabled) in capabilities.into_iter().zip(enabled) {
            if was_enabled { gl.enable(cap); } else { gl.disable(cap); }
        }
        gl.blend_func_separate(blend[0], blend[1], blend[2], blend[3]);
        gl.blend_equation_separate(blend[4], blend[5]);
        gl.color_mask(mask[0], mask[1], mask[2], mask[3]);
        gl.bind_texture(glow::TEXTURE_2D, texture);
        gl.bind_sampler(0, sampler);
        gl.active_texture(active_texture);
        gl.use_program(program);
        gl.bind_vertex_array(vao);
        gl.bind_buffer(glow::ARRAY_BUFFER, buffer);
        gl.bind_framebuffer(glow::DRAW_FRAMEBUFFER, draw_framebuffer);
        gl.bind_framebuffer(glow::READ_FRAMEBUFFER, read_framebuffer);
        gl.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }

    // Ported from jawaka-menu/main.c: jw__draw_ingame_underlay,
    // jw__draw_ingame_list_focus and jw__render_ingame_menu. Same theme palette,
    // dimmed paused frame, translucent panels, pill focus and label contrast.
    fn palette()->[[f32;4];6] {
        let defaults=["#D3D0CC","#993B41","#993B41","#D3D0CC","#993B41","#CABCBA"];
        std::array::from_fn(|i|{
            let value=std::env::var(["RUFFLE_THEME_BG","RUFFLE_THEME_TEXT","RUFFLE_THEME_HIGHLIGHT","RUFFLE_THEME_SELECTED","RUFFLE_THEME_HINT","RUFFLE_THEME_ACCENT"][i]).unwrap_or(defaults[i].into());
            let value=value.trim().trim_start_matches('#');
            let n=u32::from_str_radix(value,16).unwrap_or(u32::from_str_radix(&defaults[i][1..],16).unwrap());
            [((n>>16)&255) as f32/255.,((n>>8)&255) as f32/255.,(n&255) as f32/255.,1.]
        })
    }
    unsafe fn rounded(&self,rect:(f32,f32,f32,f32),radius:f32,color:[f32;4]) {
        self.rounded_corners(rect,radius,color,[true;4]);
    }
    unsafe fn rounded_corners(&self,rect:(f32,f32,f32,f32),radius:f32,color:[f32;4],corners:[bool;4]) {
        let (x,y,w,h)=rect;let r=radius.min(w/2.).min(h/2.);
        let mut data=Vec::<f32>::new();
        let mut edge=Vec::new();
        for (i,(cx,cy,start)) in [(x+w-r,y+r,-90.),(x+w-r,y+h-r,0.),(x+r,y+h-r,90.),(x+r,y+r,180.)].into_iter().enumerate() {
            if !corners[i]{edge.push([(x+w,y),(x+w,y+h),(x,y+h),(x,y)][i]);continue;}
            for i in 0..=8 {let a=(start+i as f32*90./8.).to_radians();edge.push((cx+r*a.cos(),cy+r*a.sin()));}
        }
        for i in 0..edge.len(){for (px,py) in [(x+w/2.,y+h/2.),edge[i],edge[(i+1)%edge.len()]]{data.extend([px/480.-1.,1.-py/360.,0.,0.]);}}
        self.gl.buffer_data_u8_slice(glow::ARRAY_BUFFER,bytemuck::cast_slice(&data),glow::STREAM_DRAW);
        self.gl.uniform_4_f32(self.color.as_ref(),color[0],color[1],color[2],color[3]);self.gl.uniform_1_i32(self.textured.as_ref(),0);
        self.gl.draw_arrays(glow::TRIANGLES,0,(data.len()/4) as i32);
    }
    unsafe fn leaf_background(&mut self,title:&str,subtitle:&str,back_label:&str) {
        self.quad((0.,0.,960.,720.),[0.,0.,0.,0.59],None);
        let mut bg=self.palette[0];bg[3]=200./255.;
        self.rounded((7.,4.,946.,74.),14.,bg);
        self.rounded((7.,83.,946.,548.),14.,bg);
        self.live_label_width(0,title,64.,23.,5.,720.,false,self.palette[1]);
        let stamp=crate::session::experience::date(crate::session::experience::now());
        self.rounded((820.,10.,120.,60.),30.,self.palette[5]);
        self.live_label_width(45,&stamp[11..16],30.,832.,25.,96.,true,self.palette[4]);
        self.live_label_width(18,subtitle,32.,23.,92.,910.,false,self.palette[4]);
        self.leaf_footer(back_label);
    }
    unsafe fn leaf_row(&mut self,cache:usize,text:&str,y:f32,h:f32,selected:bool) {
        if selected{
            let radius=h/2.*[1.,0.25,0.,1.][self.pill_shape as usize];
            let corners=if self.pill_shape==3{[false,true,false,true]}else{[true;4]};
            self.rounded_corners((13.,y,916.,h),radius,self.palette[2],corners);
        }
        self.live_label_width(cache,text,40.,23.,y+(h-44.)/2.,880.,false,self.palette[if selected{3}else{1}]);
    }
    unsafe fn leaf_footer(&mut self,back_label:&str) {
        self.rounded((20.,642.,315.,58.),29.,self.palette[5]);
        self.rounded((620.,642.,320.,58.),29.,self.palette[5]);
        self.rounded_corners((30.,650.,60.,44.),22.,self.palette[2],[false,true,false,true]);
        self.rounded_corners((630.,650.,48.,44.),22.,self.palette[2],[false,true,false,true]);
        self.rounded_corners((802.,650.,48.,44.),22.,self.palette[2],[false,true,false,true]);
        self.live_label_width(46,"↑↓",35.,30.,651.,55.,true,self.palette[3]);
        self.live_label_width(47,"移动  ←→ 调整",28.,98.,655.,228.,false,self.palette[4]);
        self.live_label_width(48,"B",36.,636.,650.,36.,true,self.palette[3]);
        self.live_label_width(49,back_label,28.,678.,655.,122.,false,self.palette[4]);
        self.live_label_width(50,"A",36.,808.,650.,36.,true,self.palette[3]);
        self.live_label_width(51,"确定",28.,854.,655.,76.,false,self.palette[4]);
    }
    unsafe fn draw_experience(&mut self,state:&Session) {
        use crate::session::experience::Page;
        let u=&state.experience;
        let subtitle=match u.page{
            Page::Saves=>if u.started{"A 读取并重启游戏；Y 将当前进度复制到选中槽"}else{"发现游戏进度，请先选择存档；空槽开始新进度"},
            Page::Settings=>"仅此游戏 · 调整后自动保存",Page::Manager=>"←→ 选择游戏 · 清理前会再次确认",Page::Confirm=>"请选择确认或取消",_=>""
        };
        self.leaf_background(&u.heading(),subtitle,"返回");
        let rows=u.rows();
        if u.page==Page::Confirm {
            let description=if u.pending>=20{format!("将当前游戏进度复制到存档 {}，覆盖该槽原有进度？",u.pending-19)}
                else if u.pending>=10{format!("先保存当前进度，再重启并读取存档 {}？",u.pending-9)}
                else {match u.pending{1=>"清空所选游戏的五个存档？",2=>"恢复所选游戏的默认按键？",3=>"清空所有游戏的全部存档？",_=>"重置所有按键配置与用户模板？"}.into()};
            self.live_label_width(19,&description,32.,23.,190.,910.,false,self.palette[1]);
            let note=if u.pending>=20 {"仅复制游戏原生进度；覆盖时保留上次有效备份。"}
                else if u.pending>=10 {"将重新启动游戏，读取所选槽位的原生进度。"}else{"清理的数据会移入本地备份目录。"};
            self.live_label_width(20,note,22.,60.,250.,830.,false,self.palette[1]);
        }
        for (i,row) in rows.iter().enumerate(){self.leaf_row(i+1,row,if u.page==Page::Confirm{340.+i as f32*75.}else{143.+i as f32*59.},55.,i==u.selected);}
        if u.page==Page::Saves {
            self.live_label_width(21,"保存游戏自身支持的进度；不包含任意画面的即时快照",21.,55.,540.,850.,false,self.palette[1]);
            self.live_label_width(22,"时间为北京时间，显示最后一次进度变化的时间。",20.,55.,575.,850.,false,self.palette[1]);
        }
        if !u.message.is_empty(){self.live_label_width(23,&u.message,21.,55.,615.,850.,false,self.palette[1]);}
    }

    unsafe fn draw_cursor(&self, settings: crate::session::cursor::Settings, position: (f32,f32)) {
        for mesh in crate::session::cursor::meshes(settings,position) {
            let data: Vec<f32> = mesh.vertices.iter().flat_map(|v| [v[0]/480.-1.,1.-v[1]/360.,0.,0.]).collect();
            self.gl.buffer_data_u8_slice(glow::ARRAY_BUFFER,bytemuck::cast_slice(&data),glow::STREAM_DRAW);
            self.gl.uniform_4_f32(self.color.as_ref(),mesh.color[0],mesh.color[1],mesh.color[2],mesh.color[3]);
            self.gl.uniform_1_i32(self.textured.as_ref(),0);
            self.gl.draw_arrays(glow::TRIANGLES,0,mesh.vertices.len() as i32);
        }
    }

    unsafe fn live_label(&mut self, slot: usize, label: &str, size: f32, x: f32, y: f32, color: [f32;4]) {
        self.live_label_width(slot,label,size,x,y,830.,false,color);
    }
    unsafe fn prepare_label(&mut self,slot:usize,label:&str,size:f32)->Option<(f32,f32)> {
        let key = format!("{size}:{label}");
        while self.live.len() <= slot {
            if let Ok(t) = Self::text(&self.gl, &self.font, " ", size) { self.live.push((String::new(),t)); } else { return None; }
        }
        if self.live[slot].0 != key {
            match Self::text(&self.gl, &self.font, label, size) {
                Ok(t) => { self.gl.delete_texture(self.live[slot].1.texture); self.live[slot] = (key,t); },
                Err(e) => { log::error!("menu text: {e}"); return None; }
            }
        }
        Some((self.live[slot].1.width as f32,self.live[slot].1.height as f32))
    }
    unsafe fn live_chip(&mut self,slot:usize,label:&str,size:f32,anchor:(f32,f32),max_width:f32,centered:bool,color:[f32;4]) {
        if label.is_empty(){return;}
        let Some((width,height))=self.prepare_label(slot,label,size) else{return;};
        let scale=(max_width/width).min(1.);
        let chip_w=width*scale+10.;let chip_h=height*scale+6.;
        let x=if centered{anchor.0-chip_w/2.}else{anchor.0};let y=anchor.1;
        self.rounded((x,y,chip_w,chip_h),4.,[0.,0.,0.,0.75]);
        self.live_label_width(slot,label,size,x+5.,y+3.,max_width,false,color);
    }
    unsafe fn live_label_width(&mut self, slot: usize, label: &str, size: f32, x: f32, y: f32, width: f32, centered: bool, color: [f32;4]) {
        if self.prepare_label(slot,label,size).is_none(){return;}
        let t = &self.live[slot].1;
        let scale = (width / t.width as f32).min(1.);
        let x=if centered {x+(width-t.width as f32*scale)/2.} else {x};
        let data: [f32;24] = {
            let x0=x/480.-1.; let x1=(x+t.width as f32*scale)/480.-1.;
            let y0=1.-y/360.; let y1=1.-(y+t.height as f32*scale)/360.;
            [x0,y0,0.,0.,x1,y0,1.,0.,x0,y1,0.,1.,x0,y1,0.,1.,x1,y0,1.,0.,x1,y1,1.,1.]
        };
        self.gl.buffer_data_u8_slice(glow::ARRAY_BUFFER, bytemuck::cast_slice(&data), glow::STREAM_DRAW);
        self.gl.uniform_4_f32(self.color.as_ref(),color[0],color[1],color[2],color[3]);
        self.gl.uniform_1_i32(self.textured.as_ref(),1);
        self.gl.bind_texture(glow::TEXTURE_2D,Some(t.texture)); self.gl.draw_arrays(glow::TRIANGLES,0,6);
    }

    unsafe fn draw_controls(&mut self, state: &Session) {
        let controls = &state.controls;
        self.quad((0.,0.,960.,720.),self.palette[0],None);
        self.live_label(0, controls.title(),36.,65.,40.,self.palette[1]);
        if controls.page == crate::session::controls::Page::Cursor {
            self.live_label(12,"预览",21.,735.,51.,self.palette[4]);
            self.draw_cursor(controls.cursor_draft,(850.,61.));
        }
        let lines = controls.lines();
        use crate::session::controls::{Page,CATEGORIES,GRID_COLS};
        if controls.page==Page::Picker {
            self.live_label_width(1,"L1",22.,18.,111.,52.,true,[0.85,0.91,0.96,1.]);
            self.live_label_width(2,"R1",22.,890.,111.,52.,true,[0.85,0.91,0.96,1.]);
            for (i,name) in CATEGORIES.iter().enumerate() {
                let x=77.+i as f32*90.;
                self.quad((x,101.,84.,43.),if i==controls.category {self.palette[2]} else {self.palette[5]},None);
                self.live_label_width(3+i,name,22.,x+4.,110.,76.,true,self.palette[if i==controls.category{3}else{1}]);
            }
            let step=(420./lines.len().div_ceil(GRID_COLS) as f32).min(120.);
            for (i,line) in lines.iter().enumerate() {
                let x=45.+(i%GRID_COLS) as f32*176.; let y=165.+(i/GRID_COLS) as f32*step;
                self.quad((x,y,166.,step-7.),if i==controls.row {self.palette[2]} else {self.palette[5]},None);
                self.live_label_width(14+i,line,25.,x+9.,y+(step-7.-29.)/2.,148.,true,self.palette[if i==controls.row{3}else{1}]);
            }
            self.live_label(50,&controls.detail(),22.,45.,602.,self.palette[4]);
            self.live_label(51,&controls.status,21.,45.,641.,[1.,0.8,0.4,1.]);
            self.live_label(52,controls.hint(),22.,45.,679.,self.palette[4]);
            return;
        }
        if controls.page==Page::Edit {
            for (i,line) in lines.iter().enumerate() {
                let x=45.+(i%GRID_COLS) as f32*176.; let y=104.+(i/GRID_COLS) as f32*78.;
                let color=self.palette[if i==controls.row{3}else{1}];
                self.quad((x,y,166.,70.),if i==controls.row{self.palette[2]}else{self.palette[5]},None);
                if let Some((label,value))=line.split_once('：') {
                    self.live_label_width(1+i*2,label,21.,x+9.,y+8.,148.,true,color);
                    self.live_label_width(2+i*2,value,24.,x+9.,y+35.,148.,true,color);
                } else { self.live_label_width(1+i*2,line,23.,x+9.,y+23.,148.,true,color); }
            }
            self.live_label_width(54,&format!("方案 {} · {}",controls.game_slot+1,controls.detail()),21.,45.,594.,870.,true,self.palette[4]);
            self.live_label_width(55,&controls.status,21.,45.,636.,870.,true,[1.,0.8,0.4,1.]);
            self.live_label_width(56,controls.hint(),22.,45.,679.,870.,true,self.palette[4]);
            return;
        }
        let start = (controls.row/7)*7;
        for (i,line) in lines.iter().enumerate().skip(start).take(7) {
            let y = 116.+(i-start) as f32*60.;
            if i == controls.row { self.quad((50.,y,860.,55.),self.palette[2],None); }
            self.live_label(1+i-start,line,28.,70.,y+10.,self.palette[if i==controls.row{3}else{1}]);
        }
        self.live_label(8,&format!("{} / {}",controls.row+1,lines.len()),20.,790.,560.,self.palette[4]);
        self.live_label(9,&controls.detail(),21.,65.,598.,self.palette[4]);
        self.live_label(10,&controls.status,21.,65.,636.,[1.,0.8,0.4,1.]);
        self.live_label(11,controls.hint(),21.,65.,679.,self.palette[4]);
    }

    /// Diagnostic-only framebuffer readback, never called during normal play.
    pub unsafe fn capture(&self, path: &Path, width: u32, height: u32) -> Result<(), String> {
        let gl = &self.gl;
        let framebuffer = gl.get_parameter_framebuffer(glow::READ_FRAMEBUFFER_BINDING);
        let buffer = gl.get_parameter_buffer(glow::PIXEL_PACK_BUFFER_BINDING);
        let alignment = gl.get_parameter_i32(glow::PACK_ALIGNMENT);
        gl.bind_framebuffer(glow::READ_FRAMEBUFFER, None);
        gl.bind_buffer(glow::PIXEL_PACK_BUFFER, None);
        gl.pixel_store_i32(glow::PACK_ALIGNMENT, 4);
        gl.finish();
        let mut pixels = vec![0; width as usize * height as usize * 4];
        gl.read_pixels(0, 0, width as i32, height as i32, glow::RGBA, glow::UNSIGNED_BYTE, glow::PixelPackData::Slice(&mut pixels));
        gl.bind_framebuffer(glow::READ_FRAMEBUFFER, framebuffer);
        gl.bind_buffer(glow::PIXEL_PACK_BUFFER, buffer);
        gl.pixel_store_i32(glow::PACK_ALIGNMENT, alignment);
        let stride = (width * 3 + 3) & !3;
        let mut bmp = vec![0u8; 54 + (stride * height) as usize];
        let file_size = bmp.len() as u32;
        bmp[0..2].copy_from_slice(b"BM");
        bmp[2..6].copy_from_slice(&file_size.to_le_bytes());
        bmp[10..14].copy_from_slice(&54u32.to_le_bytes());
        bmp[14..18].copy_from_slice(&40u32.to_le_bytes());
        bmp[18..22].copy_from_slice(&width.to_le_bytes());
        bmp[22..26].copy_from_slice(&height.to_le_bytes());
        bmp[26..28].copy_from_slice(&1u16.to_le_bytes());
        bmp[28..30].copy_from_slice(&24u16.to_le_bytes());
        for y in 0..height as usize {
            for x in 0..width as usize {
                let from = (y * width as usize + x) * 4;
                let to = 54 + y * stride as usize + x * 3;
                bmp[to..to+3].copy_from_slice(&[pixels[from+2], pixels[from+1], pixels[from]]);
            }
        }
        std::fs::write(path, bmp).map_err(|e| e.to_string())
    }
}

impl Drop for PauseMenu {
    fn drop(&mut self) {
        unsafe {
            for text in &self.texts { self.gl.delete_texture(text.texture); }
            for (_,text) in &self.live { self.gl.delete_texture(text.texture); }
            self.gl.delete_vertex_array(self.vao);
            self.gl.delete_buffer(self.vertices);
            self.gl.delete_program(self.program);
        }
    }
}
