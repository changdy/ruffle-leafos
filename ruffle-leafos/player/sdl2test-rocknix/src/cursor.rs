//! Host cursor appearance and a monotonic, movement-only idle deadline.
//! Drawing uses frontend overlay triangles, independently of Flash stage scaling.
use std::time::Duration;

pub const HIDE_AFTER: Duration = Duration::from_millis(3500);

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Style { #[default] RingDot, Crosshair, Dot, Square }
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Color { #[default] White, Black, Red, Yellow, Green, Cyan, Magenta }
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Size { Small, #[default] Medium, Large, ExtraLarge }

impl Style {
    pub const ALL: [Self;4] = [Self::RingDot,Self::Crosshair,Self::Dot,Self::Square];
    pub fn id(self) -> &'static str { match self { Self::RingDot=>"ring-dot",Self::Crosshair=>"crosshair",Self::Dot=>"dot",Self::Square=>"square" } }
    pub fn name(self) -> &'static str { match self { Self::RingDot=>"空心圆＋中心点",Self::Crosshair=>"十字",Self::Dot=>"圆点",Self::Square=>"方框" } }
    pub fn parse(s: &str) -> Option<Self> {
        // Accept saved arrow configurations without retaining an arrow model
        // or rewriting a user's file merely because it was opened.
        if s=="arrow" {Some(Self::RingDot)} else {Self::ALL.into_iter().find(|v|v.id()==s)}
    }
    pub fn next(self, step: i32) -> Self { Self::ALL[(Self::ALL.iter().position(|v|*v==self).unwrap() as i32+step).rem_euclid(4) as usize] }
}
impl Color {
    pub const ALL: [Self;7] = [Self::White,Self::Black,Self::Red,Self::Yellow,Self::Green,Self::Cyan,Self::Magenta];
    pub fn id(self) -> &'static str { match self { Self::White=>"white",Self::Black=>"black",Self::Red=>"red",Self::Yellow=>"yellow",Self::Green=>"green",Self::Cyan=>"cyan",Self::Magenta=>"magenta" } }
    pub fn name(self) -> &'static str { match self { Self::White=>"白色",Self::Black=>"黑色",Self::Red=>"红色",Self::Yellow=>"黄色",Self::Green=>"绿色",Self::Cyan=>"青色",Self::Magenta=>"紫色" } }
    pub fn rgba(self) -> [f32;4] { match self { Self::White=>[1.,1.,1.,1.],Self::Black=>[0.,0.,0.,1.],Self::Red=>[1.,0.15,0.15,1.],Self::Yellow=>[1.,0.9,0.,1.],Self::Green=>[0.2,1.,0.25,1.],Self::Cyan=>[0.,0.9,1.,1.],Self::Magenta=>[1.,0.25,0.9,1.] } }
    pub fn parse(s: &str) -> Option<Self> { Self::ALL.into_iter().find(|v|v.id()==s) }
    pub fn next(self, step: i32) -> Self { Self::ALL[(Self::ALL.iter().position(|v|*v==self).unwrap() as i32+step).rem_euclid(7) as usize] }
}
impl Size {
    pub const ALL: [Self;4] = [Self::Small,Self::Medium,Self::Large,Self::ExtraLarge];
    pub fn pixels(self) -> u32 { match self { Self::Small=>16,Self::Medium=>24,Self::Large=>32,Self::ExtraLarge=>48 } }
    pub fn name(self) -> &'static str { match self { Self::Small=>"小",Self::Medium=>"中",Self::Large=>"大",Self::ExtraLarge=>"特大" } }
    pub fn parse(s: &str) -> Option<Self> { Self::ALL.into_iter().find(|v|v.pixels().to_string()==s) }
    pub fn next(self, step: i32) -> Self { Self::ALL[(Self::ALL.iter().position(|v|*v==self).unwrap() as i32+step).rem_euclid(4) as usize] }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Settings { pub style: Style, pub color: Color, pub size: Size }

#[derive(Default)]
pub struct Idle { last_moved: Duration, position: Option<(f32,f32)> }
impl Idle {
    pub fn rebase(&mut self, position: (f32,f32)) { self.position=Some(position); }
    /// Only call for actual pointer movement; clicks, keys, ticks and menu
    /// navigation deliberately do not extend the deadline.
    pub fn moved(&mut self, position: (f32,f32), now: Duration) {
        if self.position != Some(position) { self.position = Some(position); self.last_moved = now; }
    }
    /// Entering a mouse mode or applying appearance provides a short preview.
    pub fn reveal(&mut self, now: Duration) { self.last_moved = now; }
    pub fn visible(&self, now: Duration) -> bool { now.saturating_sub(self.last_moved) < HIDE_AFTER }
}

pub struct Mesh { pub vertices: Vec<[f32;2]>, pub color: [f32;4] }
fn rect(v: &mut Vec<[f32;2]>, x: f32, y: f32, w: f32, h: f32) {
    v.extend([[x,y],[x+w,y],[x,y+h],[x,y+h],[x+w,y],[x+w,y+h]]);
}
fn disk(v: &mut Vec<[f32;2]>, radius: f32) {
    for i in 0..24 {
        let a=i as f32*std::f32::consts::TAU/24.; let b=(i+1) as f32*std::f32::consts::TAU/24.;
        v.extend([[0.,0.],[a.cos()*radius,a.sin()*radius],[b.cos()*radius,b.sin()*radius]]);
    }
}

fn ring(v: &mut Vec<[f32;2]>, outer: f32, inner: f32) {
    for i in 0..48 {
        let a=i as f32*std::f32::consts::TAU/48.;let b=(i+1) as f32*std::f32::consts::TAU/48.;
        let (ac,as_)=(a.cos(),a.sin());let (bc,bs)=(b.cos(),b.sin());
        v.extend([[ac*outer,as_*outer],[bc*outer,bs*outer],[ac*inner,as_*inner],
                  [ac*inner,as_*inner],[bc*outer,bs*outer],[bc*inner,bs*inner]]);
    }
}

/// Contrast outline first, then the chosen fill; all styles have a centered
/// hotspot. The ring stays hollow and its center dot covers one screen pixel.
pub fn meshes(settings: Settings, position: (f32,f32)) -> [Mesh;2] {
    let mut outline=Vec::new(); let mut fill=Vec::new();
    match settings.style {
        Style::RingDot => { ring(&mut outline,12.,9.);ring(&mut fill,11.,10.); }
        Style::Crosshair => { rect(&mut outline,-12.,-3.,24.,6.); rect(&mut outline,-3.,-12.,6.,24.); rect(&mut fill,-10.,-1.,20.,2.); rect(&mut fill,-1.,-10.,2.,20.); }
        Style::Dot => { disk(&mut outline,12.); disk(&mut fill,9.); }
        Style::Square => {
            rect(&mut outline,-12.,-12.,24.,24.);
            // Hollow center keeps small game targets visible.
            rect(&mut fill,-10.,-10.,20.,3.); rect(&mut fill,-10.,7.,20.,3.);
            rect(&mut fill,-10.,-7.,3.,14.); rect(&mut fill,7.,-7.,3.,14.);
            outline.clear(); rect(&mut outline,-12.,-12.,24.,7.); rect(&mut outline,-12.,5.,24.,7.);
            rect(&mut outline,-12.,-5.,7.,10.); rect(&mut outline,5.,-5.,7.,10.);
        }
    }
    let scale=settings.size.pixels() as f32/24.;
    for vertex in outline.iter_mut().chain(fill.iter_mut()) { vertex[0]=position.0+vertex[0]*scale; vertex[1]=position.1+vertex[1]*scale; }
    if settings.style==Style::RingDot {
        // Append after size scaling and snap to the pixel containing the click
        // coordinate so fractional movement cannot blur the dot into 4 pixels.
        rect(&mut fill,position.0.floor(),position.1.floor(),1.,1.);
    }
    [Mesh {vertices:outline,color:if settings.color==Color::Black {[1.,1.,1.,1.]} else {[0.,0.,0.,1.]}},Mesh{vertices:fill,color:settings.color.rgba()}]
}

#[cfg(test)]
mod tests {
    use super::*;
    fn at(ms:u64)->Duration { Duration::from_millis(ms) }
    #[test]
    fn idle_hides_at_3500ms_and_same_position_does_not_keep_it_awake() {
        let mut idle=Idle::default(); idle.moved((10.,20.),at(0));
        assert!(idle.visible(at(3499))); idle.moved((10.,20.),at(3400)); assert!(!idle.visible(at(3500)));
        idle.moved((11.,20.),at(6000)); assert!(idle.visible(at(6000))); assert!(idle.visible(at(9499))); assert!(!idle.visible(at(9500)));
    }
    #[test]
    fn deadline_depends_on_monotonic_time_not_game_frames() {
        for hz in [30,60] {
            let idle=Idle::default(); for frame in 0..hz*4 { let now=Duration::from_secs_f64(frame as f64/hz as f64); assert_eq!(idle.visible(now),now<HIDE_AFTER); }
        }
    }
    #[test]
    fn appearance_reveal_does_not_replay_or_modify_pointer_position() {
        let mut idle=Idle::default(); idle.moved((3.,7.),at(0)); assert!(!idle.visible(at(5000))); idle.reveal(at(5000));
        assert_eq!(idle.position,Some((3.,7.))); assert!(idle.visible(at(8499))); assert!(!idle.visible(at(8500)));
    }
    #[test]
    fn viewport_rebase_does_not_wake_but_the_next_real_movement_does() {
        let mut idle=Idle::default(); idle.moved((400.,300.),at(0)); idle.rebase((200.,200.));
        assert!(!idle.visible(at(5000))); idle.moved((400.,300.),at(5000)); assert!(idle.visible(at(5000)));
    }
    #[test]
    fn all_shapes_and_sizes_have_finite_geometry_and_contrast_outlines() {
        for style in Style::ALL { for size in Size::ALL { for color in Color::ALL {
            let [outline,fill]=meshes(Settings{style,size,color},(200.,200.));
            assert!(!fill.vertices.is_empty()); assert_eq!(fill.vertices.len()%3,0); assert_ne!(outline.color,fill.color);
            assert!(outline.vertices.iter().chain(fill.vertices.iter()).flatten().all(|v|v.is_finite() && (152. ..=248.).contains(v)));
        } } }
    }
    #[test]
    fn drawing_size_scales_around_the_same_hotspot() {
        for style in Style::ALL {
            let small=meshes(Settings{style,size:Size::Small,..Settings::default()},(100.,100.));
            let large=meshes(Settings{style,size:Size::Large,..Settings::default()},(100.,100.));
            for (a,b) in small[0].vertices.iter().zip(&large[0].vertices) { for n in 0..2 { assert!(((a[n]-100.)*2.-(b[n]-100.)).abs()<0.001); } }
        }
    }
    #[test]
    fn removed_arrow_loads_as_ring_but_is_not_a_selectable_or_saved_style() {
        assert_eq!(Style::parse("arrow"),Some(Style::RingDot));
        assert_eq!(Style::default(),Style::RingDot);
        assert!(Style::ALL.into_iter().all(|style|style.id()!="arrow"));
        for style in Style::ALL {assert_eq!(Style::parse(style.id()),Some(style));}
        assert_eq!(Style::parse("unknown"),None);
    }
    fn covers(mesh:&Mesh,point:[f32;2])->bool {
        mesh.vertices.chunks_exact(3).any(|t| {
            let cross=|a:[f32;2],b:[f32;2]|(b[0]-a[0])*(point[1]-a[1])-(b[1]-a[1])*(point[0]-a[0]);
            let signs=[cross(t[0],t[1]),cross(t[1],t[2]),cross(t[2],t[0])];
            signs.iter().all(|v|*v>=0.)||signs.iter().all(|v|*v<=0.)
        })
    }
    #[test]
    fn ring_has_a_hollow_interior_and_one_center_pixel_at_every_size() {
        let position=(100.25,100.75);
        for size in Size::ALL {
            let [outline,fill]=meshes(Settings{style:Style::RingDot,size,..Settings::default()},position);
            let mut interior_pixels=Vec::new();
            for y in 97..104 {for x in 97..104 {
                let p=[x as f32+0.5,y as f32+0.5];
                assert!(!covers(&outline,p));
                if covers(&fill,p){interior_pixels.push((x,y));}
            }}
            assert_eq!(interior_pixels,vec![(100,100)]);
            let scale=size.pixels() as f32/24.;
            assert!(covers(&fill,[position.0+10.5*scale,position.1]));
            assert!(!covers(&fill,[position.0+8.*scale,position.1]));
        }
    }
}
