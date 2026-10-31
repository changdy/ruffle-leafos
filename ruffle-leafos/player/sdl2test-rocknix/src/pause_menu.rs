//! Small GL overlay. All GL state changed by the menu is restored before the
//! movie renderer runs again; changing labels use a bounded texture cache.
use crate::session::i18n::t;
use ab_glyph::{point, Font, FontArc, PxScale, ScaleFont};
use glow::HasContext;
use std::path::Path;
use std::sync::Arc;

use crate::session::Session;

struct Text {
    texture: glow::Texture,
    width: i32,
    height: i32,
}

#[derive(Clone, Copy)]
enum FooterNavigation { Adjust, Grid, List, Tabs, Back }

pub struct PauseMenu {
    gl: Arc<glow::Context>,
    program: glow::Program,
    vertices: glow::Buffer,
    vao: glow::VertexArray,
    color: Option<glow::UniformLocation>,
    textured: Option<glow::UniformLocation>,
    texts: Vec<Text>,
    font: FontArc,
    fallback_font: Option<FontArc>,
    live: Vec<(String, Text)>,
    palette: [[f32;4];6],
    pill_shape: u8,
}

impl PauseMenu {
    pub unsafe fn new(gl: Arc<glow::Context>, font_path: &Path, previews: &[[String; 4]]) -> Result<Self, String> {
        let font = FontArc::try_from_vec(std::fs::read(font_path).map_err(|e| format!("menu font: {e}"))?)
            .map_err(|_| "menu font is not a supported TrueType font".to_string())?;
        // Leaf's English theme fonts may not contain Chinese. Keep its font
        // for supported glyphs and use the bundled CJK face for missing ones.
        let fallback_font = std::env::var_os("RUFFLE_FONT_FILE")
            .filter(|_| "语言简体中文".chars().any(|c| font.glyph_id(c).0 == 0))
            .and_then(|path| std::fs::read(path).ok())
            .and_then(|bytes| FontArc::try_from_vec(bytes).ok());
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
            (t("暂停"), 40.0), (t("继续游戏"), 32.0),
            (t("通用控制：混合"), 32.0), (t("通用控制：键盘"), 32.0),
            (t("通用控制：鼠标"), 32.0), (t("返回 Leaf"), 32.0),
            (t("上下选择  左右预览  A 应用  B 返回"), 23.0),
            (t("混合：十字键和按钮操作游戏，摇杆移动鼠标，L 左键 R 右键"), 20.0),
            (t("键盘：使用游戏按键配置；无自定义时 A 空格 B 回车"), 20.0),
            (t("鼠标：摇杆或十字键移动，A 左键 B 右键，可按住拖拽"), 20.0),
            (t("鼠标速度：慢   Y 光标外观"), 29.0), (t("鼠标速度：标准   Y 光标外观"), 29.0), (t("鼠标速度：快   Y 光标外观"), 29.0),
            (t("仅调节摇杆和十字键鼠标；重开恢复标准。A 应用，B 取消预览"), 20.0),
        ] {
            texts.push(Self::text(&gl, &font, fallback_font.as_ref(), label, pixels)?);
        }
        for name in std::iter::once(t("通用（沿用 cfg）")).chain(crate::session::profiles::PROFILES.iter().map(|p| t(p.name))) {
            texts.push(Self::text(&gl, &font, fallback_font.as_ref(), &crate::tr!("按键方案：{name}", "Control profile: {name}"), 29.0)?);
        }
        texts.push(Self::text(&gl, &font, fallback_font.as_ref(), t("预览尚未应用；A 应用，B 返回并取消未应用预览"), 20.0)?); // 27
        texts.push(Self::text(&gl, &font, fallback_font.as_ref(), t("左键保持：M+十字下解除；暂停时释放"), 21.0)?); // 28
        for lines in previews {
            for line in lines { texts.push(Self::text(&gl, &font, fallback_font.as_ref(), line, 21.0)?); } // 29+
        }
        texts.push(Self::text(&gl, &font, fallback_font.as_ref(), t("自定义按键"), 30.0)?);
        Ok(Self {
            color: gl.get_uniform_location(program, "color"),
            textured: gl.get_uniform_location(program, "textured"),
            gl, program, vertices, vao, texts, font, fallback_font, live: Vec::new(),
            palette: Self::palette(),
            pill_shape: std::env::var("RUFFLE_MENU_PILL").ok().and_then(|s|s.parse().ok()).filter(|n|*n<4).unwrap_or(3),
        })
    }

    unsafe fn text(gl: &glow::Context, font: &FontArc, fallback: Option<&FontArc>, label: &str, size: f32) -> Result<Text, String> {
        let face = |c| if font.glyph_id(c).0 == 0 { fallback.unwrap_or(font) } else { font };
        let scaled = font.as_scaled(PxScale::from(size));
        let width = label.chars().map(|c| { let font=face(c); font.as_scaled(PxScale::from(size)).h_advance(font.glyph_id(c)) }).sum::<f32>().ceil() as i32 + 4;
        let ascent = fallback.map(|f| f.as_scaled(PxScale::from(size)).ascent()).unwrap_or(scaled.ascent()).max(scaled.ascent());
        let descent = fallback.map(|f| f.as_scaled(PxScale::from(size)).descent()).unwrap_or(scaled.descent()).min(scaled.descent());
        let height = (ascent - descent).ceil() as i32 + 4;
        let mut alpha = vec![0u8; (width * height) as usize];
        let mut x = 2.0;
        for c in label.chars() {
            let font=face(c);
            let scaled=font.as_scaled(PxScale::from(size));
            let glyph = font.glyph_id(c).with_scale_and_position(size, point(x, ascent + 2.0));
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
        if let Some(keyboard)=&state.virtual_keyboard {
            self.draw_virtual_keyboard(keyboard);
        } else if state.paused {
        if !state.experience.started {self.quad((0.,0.,960.,720.),self.palette[0],None);}
        if state.experience.page!=crate::session::experience::Page::Home {
            self.draw_experience(state);
        } else if state.controls.visible {
            self.draw_controls(state);
        } else {
        self.leaf_background(&state.experience.title,if state.experience.started{t("Flash · 已暂停")}else{"Flash"},t("继续游戏"),FooterNavigation::Adjust);
        let slot=state.controls.game_slot;
        let rows=[t("继续"),t("按键设置"),t("鼠标设置"),t("保存游戏进度"),t("游戏存档"),t("画面设置"),t("操作说明"),t("关于"),t("语言"),t("保存并退出")];
        let details=[String::new(),crate::tr!("◀ 方案 {} ▶", "◀ Profile {} ▶",slot+1),
            String::new(),t("游戏内进度").into(),crate::tr!("存档 {} / 5", "Slot {} / 5",state.experience.active+1),String::new(),String::new(),String::new(),String::new(),String::new()];
        for (i,line) in rows.iter().enumerate() {
            self.leaf_row(i+1,line,137.+i as f32*46.,44.,i==state.selected);
            if !details[i].is_empty(){self.live_label_width(30+i,&details[i],31.,600.,141.+i as f32*46.,315.,false,self.palette[if i==state.selected{3}else{4}]);}
        }
        let hint=match state.selected {1=>t("左右切换方案 · A 进入编辑"),6=>t("查看游戏组合键、菜单操作与按键编辑方法"),7=>t("认识 Ruffle Flash，查看项目主页与开源致谢"),_=>t("操作方法见「操作说明」")};
        self.live_label_width(16,hint,20.,23.,606.,910.,false,self.palette[4]);
        }
        } else if state.drag_locked {
            self.live_chip(61,t("左键保持 · M+十字下解除"),22.,(10.,if state.experience.settings.fps{48.}else{10.}),600.,false,[1.,0.8,0.4,1.]);
        }
        if let Some((level,_))=&state.system_volume {
            let label=crate::tr!("系统音量 {}%", "System volume {}%",level);
            self.live_chip(63,&label,23.,(600.,10.),350.,false,[0.96,0.99,1.,1.]);
        }
        if let Some((message,_))=&state.toast {
                self.live_chip(60,message,25.,(480.,658.),920.,true,[0.96,0.99,1.,1.]);
        }
        if !state.paused && state.virtual_keyboard.is_none() {
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

    unsafe fn draw_virtual_keyboard(&mut self, keyboard: &crate::session::virtual_keyboard::Editor) {
        self.quad((0.,0.,960.,720.),self.palette[0],None);
        self.live_label_width(150,t("输入文字"),35.,30.,18.,900.,false,self.palette[1]);
        self.rounded((30.,77.,900.,75.),10.,self.palette[2]);
        self.live_label_width(151,&keyboard.preview(),31.,47.,97.,866.,false,self.palette[3]);
        self.live_label_width(152,t("A 输入 · B 退格 · X 空格 · Select 大小写"),23.,30.,164.,900.,true,self.palette[4]);
        for (i,key) in keyboard.keys().iter().enumerate() {
            let selected=keyboard.row==key.row&&keyboard.col==key.col;
            self.rounded(key.rect,8.,self.palette[if selected {2}else{5}]);
            self.live_label_width(160+i,&key.label,29.,key.rect.0+5.,key.rect.1+18.,key.rect.2-10.,true,self.palette[if selected {3}else{1}]);
        }
        self.live_label_width(153,t("L1 / R1 移动光标 · L2 符号 · M 帮助"),22.,30.,601.,900.,true,self.palette[4]);
        self.live_label_width(154,t("Y 取消                         Start 确认"),27.,30.,660.,900.,true,self.palette[1]);
        if keyboard.help {
            self.quad((0.,0.,960.,720.),[0.,0.,0.,0.65],None);
            self.rounded((65.,180.,830.,365.),18.,self.palette[0]);
            let lines=[t("十字键选择按键，A 输入，B 退格，X 空格。"),
                t("Select 切换大小写，L2 切换符号。"),t("L1 / R1 移动文字光标；多行框可用 R2 换行。"),
                t("Start 确认并回到游戏，Y 取消全部修改。"),t("按 M 或 A 关闭帮助。")];
            for (i,line) in lines.iter().enumerate() {self.live_label_width(205+i,line,25.,90.,215.+i as f32*60.,780.,false,self.palette[1]);}
        }
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
    unsafe fn leaf_background(&mut self,title:&str,subtitle:&str,back_label:&str,navigation:FooterNavigation) {
        self.quad((0.,0.,960.,720.),[0.,0.,0.,0.59],None);
        let mut bg=self.palette[0];bg[3]=200./255.;
        self.rounded((7.,4.,946.,74.),14.,bg);
        self.rounded((7.,83.,946.,548.),14.,bg);
        self.live_label_width(0,title,64.,23.,5.,720.,false,self.palette[1]);
        let stamp=crate::session::experience::date(crate::session::experience::now());
        self.rounded((820.,10.,120.,60.),30.,self.palette[5]);
        self.live_label_width(45,&stamp[11..16],30.,832.,25.,96.,true,self.palette[4]);
        self.live_label_width(18,subtitle,32.,23.,92.,910.,false,self.palette[4]);
        self.leaf_footer(back_label,navigation);
    }
    unsafe fn leaf_row(&mut self,cache:usize,text:&str,y:f32,h:f32,selected:bool) {
        if selected{
            let radius=h/2.*[1.,0.25,0.,1.][self.pill_shape as usize];
            let corners=if self.pill_shape==3{[false,true,false,true]}else{[true;4]};
            self.rounded_corners((13.,y,916.,h),radius,self.palette[2],corners);
        }
        self.live_label_width(cache,text,40.,23.,y+(h-44.)/2.,880.,false,self.palette[if selected{3}else{1}]);
    }
    unsafe fn leaf_footer(&mut self,back_label:&str,navigation:FooterNavigation) {
        if matches!(navigation,FooterNavigation::Back) {
            self.rounded((620.,642.,320.,58.),29.,self.palette[5]);
            self.live_label_width(132,t("B 返回"),30.,644.,653.,272.,true,self.palette[4]);
            return;
        }
        let (arrows,hint,key_width,label_x)=match navigation {
            FooterNavigation::Adjust=>("↑↓",t("移动  ←→ 调整"),60.,98.),
            FooterNavigation::Grid=>("↑↓←→",t("移动焦点"),112.,150.),
            FooterNavigation::List=>("↑↓",t("移动"),60.,98.),
            FooterNavigation::Tabs=>("←→",t("切换页面"),60.,98.),
            FooterNavigation::Back=>unreachable!(),
        };
        self.rounded((20.,642.,315.,58.),29.,self.palette[5]);
        self.rounded((620.,642.,320.,58.),29.,self.palette[5]);
        self.rounded_corners((30.,650.,key_width,44.),22.,self.palette[2],[false,true,false,true]);
        self.live_label_width(46,arrows,35.,30.,651.,key_width-5.,true,self.palette[3]);
        self.live_label_width(47,hint,28.,label_x,655.,326.-label_x,false,self.palette[4]);
        if matches!(navigation,FooterNavigation::Tabs) {
            self.live_label_width(132,t("B 返回"),30.,644.,653.,272.,true,self.palette[4]);
            return;
        }
        self.rounded_corners((630.,650.,48.,44.),22.,self.palette[2],[false,true,false,true]);
        self.rounded_corners((802.,650.,48.,44.),22.,self.palette[2],[false,true,false,true]);
        self.live_label_width(48,"B",36.,636.,650.,36.,true,self.palette[3]);
        self.live_label_width(49,back_label,28.,678.,655.,122.,false,self.palette[4]);
        self.live_label_width(50,"A",36.,808.,650.,36.,true,self.palette[3]);
        self.live_label_width(51,t("确定"),28.,854.,655.,76.,false,self.palette[4]);
    }
    unsafe fn draw_experience(&mut self,state:&Session) {
        use crate::session::experience::Page;
        let u=&state.experience;
        if u.page==Page::About {self.draw_about();return;}
        if u.page==Page::Shortcuts {self.draw_shortcuts(u.shortcut_tab);return;}
        let subtitle=match u.page{
            Page::Saves=>if u.selected>=u.slots.len(){if u.started{t("A 返回")}else{t("A 返回 Leaf")}}
                else if u.slots[u.selected].broken{if u.started{t("存档损坏，无法读取；Y 用当前进度覆盖")}else{t("此槽位损坏；请先在数据管理中清理")}}
                else if !u.started{t("发现游戏进度，请先选择存档；空槽开始新进度")}
                else if u.selected==u.active{t("A 返回；Y 保存当前进度")}
                else{t("A 切换存档并重启；Y 复制当前进度")},
            Page::Settings=>t("全局共用 · 调整后自动保存"),Page::Renderer=>t("全局接口 · 切换会保存当前进度并重启"),Page::Manager=>t("管理游戏数据，或选择批量清理"),Page::ManagerGames=>t("仅显示有存档或按键配置的游戏"),Page::ManagerGame=>u.manager_game().map(|g|g.title.as_str()).unwrap_or(""),Page::ManagerBulk=>t("影响全部游戏 · 清理前会再次确认"),Page::Language=>if crate::session::i18n::is_english(){"Choose a language · Applies immediately and saves for all games"}else{"选择语言 · 立即生效并保存，所有游戏共用"},Page::Confirm=>t("请选择确认或取消"),_=>""
        };
        let navigation=if u.page==Page::Settings{FooterNavigation::Adjust}else{FooterNavigation::List};
        self.leaf_background(&u.heading(),subtitle,t("返回"),navigation);
        if u.page==Page::ManagerGames {
            let start=u.manager_visible_start();
            if u.games.is_empty(){
                self.live_label_width(19,t("暂无可管理的游戏数据"),32.,40.,205.,870.,false,self.palette[1]);
                self.live_label_width(20,t("产生存档或保存按键选择后，游戏会显示在这里。"),24.,40.,260.,870.,false,self.palette[4]);
            }
            let back_y=if u.games.is_empty(){380.}else{143.};
            for (visible,i) in (start..(start+crate::session::experience::MANAGER_VISIBLE_ROWS).min(u.games.len()+1)).enumerate(){
                let y=back_y+visible as f32*59.;
                self.leaf_row(1+visible,"",y,55.,i==u.selected);
                if let Some(game)=u.games.get(i){
                    let color=self.palette[if i==u.selected{3}else{1}];
                    let width=if game.unavailable{710.}else{880.};
                    self.live_label_width(80+visible*2,&game.title,36.,23.,y+6.,width,false,color);
                    if game.unavailable {
                        let title_width=self.live.get(80+visible*2).map(|(_,t)|t.width as f32).unwrap_or(width).min(width);
                        self.live_label_width(81+visible*2,t("（已失效）"),22.,23.+title_width+12.,y+15.,145.,false,color);
                    }
                }else{self.live_label_width(80+visible*2,t("返回"),36.,23.,y+6.,880.,false,self.palette[if i==u.selected{3}else{1}]);}
            }
            if !u.games.is_empty(){
                self.live_label_width(54,&crate::tr!("第 {} / {} 页 · L1 / R1 翻页", "Page {} / {} · L1 / R1 to page",start/7+1,(u.games.len()+7)/7),23.,23.,580.,880.,false,self.palette[4]);
            }
            if !u.message.is_empty(){self.live_label_width(23,&u.message,21.,23.,615.,880.,false,self.palette[1]);}
            return;
        }
        if u.page==Page::ManagerGame {
            if let Some(game)=u.manager_game(){
                let summary=crate::tr!("存档：{}     按键配置：{}", "Saves: {}     Controls: {}",if game.has_saves{t("有")}else{t("无")},if game.has_controls{t("有")}else{t("无")});
                self.live_label_width(19,&summary,29.,23.,160.,880.,false,self.palette[1]);
                let location=game.movie_path.as_ref().map(|p|{
                    let text=p.to_string_lossy().replace('\\',"/");
                    text.to_ascii_lowercase().find("/roms/flash/").map(|i|format!("Flash/{}",&text[i+12..]))
                        .unwrap_or_else(||p.file_name().unwrap_or_default().to_string_lossy().into_owned())
                }).unwrap_or_else(||t("旧数据未记录游戏路径").into());
                self.live_label_width(20,&location,22.,23.,208.,880.,false,self.palette[4]);
                if game.unavailable{self.live_label_width(21,t("SWF 已失效，保留的数据仍可清理。"),25.,23.,254.,880.,false,self.palette[4]);}
            }
        }
        let rows=u.rows();
        if u.page==Page::Confirm {
            let description=if u.pending>=30{crate::tr!("全局切换到 {}，保存当前进度并重启游戏？", "Switch all games to {}, save current progress and restart?",if u.pending==31{"Vulkan"}else{"OpenGL ES"})}
                else if u.pending>=20{crate::tr!("将当前游戏进度复制到存档 {}，覆盖该槽原有进度？", "Copy current progress to slot {}, replacing its progress?",u.pending-19)}
                else if u.pending>=10{crate::tr!("先保存当前进度，再重启并读取存档 {}？", "Save current progress, restart and load slot {}?",u.pending-9)}
                else {match u.pending{1=>t("清空此游戏的全部存档？"),2=>t("恢复所选游戏的默认按键？"),3=>t("清空所有游戏的全部存档？"),_=>t("重置所有游戏的按键配置？")}.into()};
            if u.app&&u.pending<=2 {if let Some(game)=u.manager_game(){self.live_label_width(24,&game.title,27.,23.,145.,910.,false,self.palette[1]);}}
            self.live_label_width(19,&description,32.,23.,190.,910.,false,self.palette[1]);
            let note=if u.pending>=30 {t("仅恢复游戏自身支持的进度，不保留当前运行画面。")}
                else if u.pending>=20 {t("仅复制游戏内进度；覆盖时保留上次有效备份。")}
                else if u.pending>=10 {t("将重新启动游戏，读取所选槽位的游戏内进度。")}else{t("清理的数据会移入本地备份目录。")};
            self.live_label_width(20,note,22.,60.,250.,830.,false,self.palette[1]);
        }
        for (i,row) in rows.iter().enumerate(){self.leaf_row(i+1,row,if u.page==Page::Confirm{340.+i as f32*75.}else if u.page==Page::ManagerGame{320.+i as f32*70.}else{143.+i as f32*59.},55.,i==u.selected);}
        if u.page==Page::Saves {
            self.live_label_width(21,t("保存游戏自身支持的进度；不包含任意画面的即时快照"),21.,55.,540.,850.,false,self.palette[1]);
            self.live_label_width(22,t("时间为北京时间，显示最后一次进度变化的时间。"),20.,55.,575.,850.,false,self.palette[1]);
        }
        if u.page==Page::Renderer {
            let details=[t("Vulkan 为实验选项，性能和兼容性因游戏而异。"),
                if u.renderer_failed{t("上次 Vulkan 启动失败，已恢复 OpenGL ES；可重试。")}
                else if u.actual_renderer==crate::session::graphics::Backend::Vulkan{t("Flash 使用 Vulkan 绘制；OpenGL ES 负责显示和菜单。")}
                else{t("默认使用 OpenGL ES；Vulkan 启动失败时自动恢复。")}];
            for (i,line) in details.iter().enumerate() {
                self.live_label_width(21+i,line,23.,55.,400.+i as f32*42.,850.,false,self.palette[1]);
            }
        }
        if !u.message.is_empty(){self.live_label_width(23,&u.message,21.,55.,615.,850.,false,self.palette[1]);}
    }

    unsafe fn draw_about(&mut self) {
        use crate::session::experience::BACKGROUND;
        self.leaf_background(t("Ruffle Flash · LeafOS 版"),&crate::tr!("{} 开发预览 · 迷你龙 Pocket 1 / LeafOS", "{} Development preview · Miniloong Pocket 1 / LeafOS",env!("CARGO_PKG_VERSION")),t("返回"),FooterNavigation::Back);
        for (i,(title,line1,line2)) in BACKGROUND.iter().enumerate() {
            let y=150.+i as f32*130.;let mut card=self.palette[5];card[3]=0.35;
            self.rounded((23.,y,910.,115.),12.,card);
            self.live_label_width(80+i*3,t(title),30.,38.,y+9.,880.,false,self.palette[1]);
            self.live_label_width(81+i*3,t(line1),25.,38.,y+49.,880.,false,self.palette[1]);
            self.live_label_width(82+i*3,t(line2),24.,38.,y+82.,880.,false,self.palette[4]);
        }
        self.live_label_width(54,t("Ruffle：MIT / Apache-2.0；本项目及 Leaf / Jawaka 菜单参考：MIT。"),22.,23.,570.,910.,false,self.palette[4]);
        self.live_label_width(55,t("完整许可与第三方声明随安装包附带。"),22.,23.,601.,910.,false,self.palette[4]);
    }

    unsafe fn draw_shortcuts(&mut self,tab:usize) {
        use crate::session::experience::{SHORTCUT_TABS,SHORTCUTS,SHORTCUT_NOTES};
        self.leaf_background(t("操作说明"),t("先看想做什么，再按右侧提示操作"),t("返回"),FooterNavigation::Tabs);
        for (i,label) in SHORTCUT_TABS.iter().enumerate() {
            let x=23.+i as f32*307.;self.leaf_tile((x,140.,296.,44.),tab==i);
            self.live_label_width(65+i,t(label),28.,x+12.,145.,272.,true,self.palette[if tab==i{3}else{1}]);
        }
        for (i,(action,keys,note)) in SHORTCUTS[tab].iter().enumerate() {
            let y=199.+i as f32*75.;let mut card=self.palette[5];card[3]=0.35;
            self.rounded((23.,y,910.,69.),10.,card);
            self.live_label_width(80+i*3,t(action),28.,38.,y+3.,410.,false,self.palette[1]);
            self.live_label_width(81+i*3,t(keys),27.,454.,y+4.,460.,false,self.palette[1]);
            self.live_label_width(82+i*3,t(note),22.,38.,y+39.,877.,false,self.palette[4]);
        }
        self.live_label_width(54,t(SHORTCUT_NOTES[tab]),22.,23.,591.,910.,false,self.palette[4]);
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
            if let Ok(t) = Self::text(&self.gl, &self.font, self.fallback_font.as_ref(), " ", size) { self.live.push((String::new(),t)); } else { return None; }
        }
        if self.live[slot].0 != key {
            match Self::text(&self.gl, &self.font, self.fallback_font.as_ref(), label, size) {
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
        use crate::session::controls::{Page,CATEGORIES,GRID_COLS};
        let subtitle=match controls.page {
            Page::Edit=>crate::tr!("L1 / R1 切换方案 · Y 连发{}", "L1 / R1 Profile · Y Turbo{}",if controls.dirty(){t(" · 未保存")}else{""}),
            Page::Picker=>t("L1 / R1 切换动作类型").into(),
            Page::Cursor=>t("鼠标速度与光标外观").into(),
            Page::Confirm=>controls.pending_detail(),
        };
        let title=if controls.page==Page::Edit {crate::tr!("按键设置 · 方案 {}", "Controls · Profile {}",controls.game_slot+1)}else{controls.title().into()};
        let navigation=match controls.page {Page::Edit|Page::Picker=>FooterNavigation::Grid,Page::Cursor=>FooterNavigation::Adjust,Page::Confirm=>FooterNavigation::List};
        self.leaf_background(&title,&subtitle,if controls.page==Page::Confirm {t("继续编辑")}else{t("返回")},navigation);
        if controls.page == crate::session::controls::Page::Cursor {
            self.live_label(12,t("预览"),21.,715.,98.,self.palette[4]);
            self.draw_cursor(controls.cursor_draft,(850.,111.));
        }
        let lines = controls.lines();
        if controls.page==Page::Confirm {
            self.live_label_width(19,&crate::tr!("方案 {} 有未保存的修改", "Profile {} has unsaved changes",controls.game_slot+1),34.,23.,185.,910.,false,self.palette[1]);
            self.live_label_width(20,t("请选择保存或舍弃，也可以返回继续编辑。"),25.,23.,245.,910.,false,self.palette[4]);
            for (i,line) in lines.iter().enumerate() {self.leaf_row(1+i,line,320.+i as f32*75.,60.,i==controls.row);}
            self.live_label_width(55,&controls.status,22.,23.,580.,910.,false,self.palette[1]);
            return;
        }
        if controls.page==Page::Picker {
            self.live_label_width(1,"L1",22.,18.,149.,52.,true,self.palette[1]);
            self.live_label_width(2,"R1",22.,890.,149.,52.,true,self.palette[1]);
            for (i,name) in CATEGORIES.iter().enumerate() {
                let x=77.+i as f32*90.;
                self.leaf_tile((x,139.,84.,43.),i==controls.category);
                self.live_label_width(65+i,t(name),22.,x+4.,148.,76.,true,self.palette[if i==controls.category{3}else{1}]);
            }
            let step=(375./lines.len().div_ceil(GRID_COLS) as f32).min(100.);
            for (i,line) in lines.iter().enumerate() {
                let x=45.+(i%GRID_COLS) as f32*176.; let y=193.+(i/GRID_COLS) as f32*step;
                self.leaf_tile((x,y,166.,step-7.),i==controls.row);
                self.live_label_width(80+i,line,25.,x+9.,y+(step-7.-29.)/2.,148.,true,self.palette[if i==controls.row{3}else{1}]);
            }
            self.live_label_width(54,&controls.detail(),22.,45.,576.,870.,false,self.palette[4]);
            self.live_label_width(55,&controls.status,21.,45.,608.,870.,false,self.palette[1]);
            return;
        }
        if controls.page==Page::Edit {
            for (i,line) in lines.iter().take(crate::session::controls::EDIT_ITEMS).enumerate() {
                let x=45.+(i%GRID_COLS) as f32*176.; let y=142.+(i/GRID_COLS) as f32*76.;
                let color=self.palette[if i==controls.row{3}else{1}];
                self.leaf_tile((x,y,166.,68.),i==controls.row);
                if let Some((label,value))=line.split_once('：').or_else(||line.split_once(':')) {
                    self.live_label_width(80+i*2,label,21.,x+9.,y+6.,148.,true,color);
                    self.live_label_width(81+i*2,value,24.,x+9.,y+34.,148.,true,color);
                } else { self.live_label_width(80+i*2,line,23.,x+9.,y+22.,148.,true,color); }
            }
            self.live_label_width(54,&if state.directions_swapped {t("本次方向已互换 · M+X 恢复").into()}else{controls.detail()},21.,430.,465.,475.,false,self.palette[4]);
            self.quad((45.,528.,870.,1.),self.palette[5],None);
            for (i,label) in [t("保存"),t("返回")].iter().enumerate() {
                let selected=controls.row==crate::session::controls::SAVE_ROW+i;
                let x=if i==0 {45.}else{650.};
                self.rounded((x,543.,265.,52.),26.,self.palette[if selected{2}else{5}]);
                self.live_label_width(130+i,label,31.,x+15.,550.,235.,true,self.palette[if selected{3}else{1}]);
            }
            self.live_label_width(55,&controls.status,21.,45.,604.,870.,false,self.palette[1]);
            return;
        }
        for (i,line) in lines.iter().enumerate() {
            self.leaf_row(1+i,line,143.+i as f32*53.,49.,i==controls.row);
        }
        self.live_label_width(54,&controls.detail(),21.,23.,535.,910.,false,self.palette[4]);
        self.live_label_width(55,controls.hint(),21.,23.,570.,910.,false,self.palette[4]);
        self.live_label_width(56,&controls.status,21.,23.,605.,910.,false,self.palette[1]);
    }

    unsafe fn leaf_tile(&self,rect:(f32,f32,f32,f32),selected:bool) {
        let radius=if selected{rect.3/2.*[1.,0.25,0.,1.][self.pill_shape as usize]}else{10.};
        let corners=if selected&&self.pill_shape==3{[false,true,false,true]}else{[true;4]};
        self.rounded_corners(rect,radius,self.palette[if selected{2}else{5}],corners);
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
