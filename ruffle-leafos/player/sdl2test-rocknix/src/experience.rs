//! Leaf menu model, five native-progress profiles, and global display options.
//! No VM snapshots: these files contain only Flash SharedObject data.
use crate::session::i18n::t;
use std::{collections::{BTreeMap,BTreeSet}, fs::{self, OpenOptions}, io::Write,
    path::{Path, PathBuf}, time::{SystemTime, UNIX_EPOCH}};
#[cfg(unix)] use std::fs::File;

pub const SAVE_SLOTS: usize = 5;
const LIMIT: usize = 32 * 1024 * 1024;
pub fn hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf29ce484222325, |h,b| (h ^ u64::from(*b)).wrapping_mul(0x100000001b3))
}
pub fn game_id(bytes: &[u8]) -> String {
    let mut reverse = bytes.to_vec(); reverse.reverse();
    format!("{:016x}{:016x}-{}", hash(bytes), hash(&reverse), bytes.len())
}
pub fn now() -> u64 { SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs() }
fn unique() -> u128 { SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos() }
fn safe(path: &Path) -> Result<(), String> {
    for p in path.ancestors() {
        if fs::symlink_metadata(p).is_ok_and(|m| m.file_type().is_symlink()) { return Err(t("数据路径不能是符号链接").into()); }
    }
    Ok(())
}
#[cfg(unix)]
fn replace(a: &Path, b: &Path) -> std::io::Result<()> { fs::rename(a,b) }
#[cfg(windows)]
fn replace(a: &Path, b: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name="kernel32")]
    extern "system" { fn MoveFileExW(a: *const u16,b: *const u16,flags:u32)->i32; }
    let a:Vec<_>=a.as_os_str().encode_wide().chain(Some(0)).collect();
    let b:Vec<_>=b.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe{MoveFileExW(a.as_ptr(),b.as_ptr(),9)}==0 {Err(std::io::Error::last_os_error())} else {Ok(())}
}
pub fn atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    safe(path)?;
    let parent=path.parent().ok_or(t("缺少数据目录"))?;
    fs::create_dir_all(parent).map_err(|e|e.to_string())?;
    let temp=parent.join(format!(".write-{}-{}",std::process::id(),unique()));
    let result=(|| -> std::io::Result<()> {
        let mut f=OpenOptions::new().create_new(true).write(true).open(&temp)?;
        f.write_all(bytes)?;f.sync_all()?;drop(f);replace(&temp,path)?;
        #[cfg(unix)] File::open(parent)?.sync_all()?;
        Ok(())
    })();
    if result.is_err(){let _=fs::remove_file(temp);}
    result.map_err(|e|e.to_string())
}

#[derive(Clone, Debug, Default)]
pub struct Snapshot { pub time: u64, pub objects: BTreeMap<String,Vec<u8>> }
impl Snapshot {
    fn encode(&self) -> Vec<u8> {
        let mut v=b"RLS1".to_vec(); v.extend(self.time.to_le_bytes());
        v.extend((self.objects.len() as u32).to_le_bytes());
        for (k,b) in &self.objects {
            v.extend((k.len() as u32).to_le_bytes());v.extend((b.len() as u32).to_le_bytes());
            v.extend(k.as_bytes());v.extend(b);
        }
        v.extend(hash(&v).to_le_bytes());v
    }
    fn decode(v:&[u8])->Result<Self,String>{
        let bad=||t("存档格式或校验失败").to_string();
        if v.len()<24 || v.len()>LIMIT || &v[..4]!=b"RLS1" {return Err(bad());}
        let end=v.len()-8;
        if hash(&v[..end])!=u64::from_le_bytes(v[end..].try_into().unwrap()){return Err(bad());}
        let time=u64::from_le_bytes(v[4..12].try_into().unwrap());
        let count=u32::from_le_bytes(v[12..16].try_into().unwrap());
        if count>4096{return Err(bad());}
        let mut at=16;let mut objects=BTreeMap::new();
        for _ in 0..count {
            if at+8>end{return Err(bad());}
            let kl=u32::from_le_bytes(v[at..at+4].try_into().unwrap()) as usize;
            let bl=u32::from_le_bytes(v[at+4..at+8].try_into().unwrap()) as usize;at+=8;
            if kl==0 || kl>16384 || at+kl+bl>end{return Err(bad());}
            let k=String::from_utf8(v[at..at+kl].to_vec()).map_err(|_|bad())?;at+=kl;
            if objects.insert(k,v[at..at+bl].to_vec()).is_some(){return Err(bad());}at+=bl;
        }
        if at!=end{return Err(bad());} Ok(Self{time,objects})
    }
}
#[derive(Clone, Debug, Default)]
pub struct Slot { pub time:u64, pub count:usize, pub recovered:bool, pub broken:bool }
impl Slot {
    pub fn detail(&self)->String {
        if self.broken {t("损坏：请在应用中清理；原文件保留").into()}
        else if self.time==0 {t("空存档").into()}
        else {format!("{}{}",date(self.time),if self.recovered {t(" · 已回退备份")} else {""})}
    }
}
// Calendar conversion uses an explicit UTC+8 offset; unaffected by host TZ.
pub fn date(seconds:u64)->String {
    let s=seconds.saturating_add(8*3600);let days=(s/86400) as i64;
    let z=days+719468;let era=z/146097;let doe=z-era*146097;
    let yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    let y=yoe+era*400;let doy=doe-(365*yoe+yoe/4-yoe/100);let mp=(5*doy+2)/153;
    let d=doy-(153*mp+2)/5+1;let m=mp+if mp<10 {3}else{-9};let y=y+i64::from(m<=2);
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}",(s%86400)/3600,(s%3600)/60,s%60)
}

#[derive(Clone, Debug)]
pub struct Progress {
    pub root:PathBuf, pub id:String, pub title:String, pub control_id:String,
    pub active:Option<usize>, data:Snapshot, pub error:Option<String>, pub recovered:bool,
    movie_path:String,
    pending_deletes:BTreeSet<String>,
}
impl Progress {
    pub fn new(root:PathBuf, bytes:&[u8], title:String, control_id:String, movie_path:String)->Self {
        Self{root,id:game_id(bytes),title,control_id,active:None,data:Snapshot::default(),error:None,recovered:false,movie_path,pending_deletes:BTreeSet::new()}
    }
    pub fn directory(&self)->PathBuf {self.root.join("games").join(&self.id)}
    pub fn register(&self)->Result<(),String>{
        self.register_path(Path::new(&self.movie_path))
    }
    pub fn register_path(&self,movie:&Path)->Result<(),String>{
        let title=self.title.replace(['\r','\n']," ");
        let path=super::data_manager::encode_movie_path(movie).unwrap_or_default();
        atomic(&self.root.join("catalog").join(format!("{}.meta",self.id)),format!("{title}\n{}\n{path}\n",self.control_id).as_bytes())
    }
    pub fn path(&self,n:usize)->PathBuf {self.directory().join(format!("slot-{}.rls",n+1))}
    fn read(&self,n:usize)->Result<(Snapshot,bool),String>{
        let path=self.path(n);safe(&path)?;
        match fs::read(&path).map_err(|e|e.to_string()).and_then(|v|Snapshot::decode(&v)) {
            Ok(v)=>Ok((v,false)),
            Err(original)=>{
                let bak=path.with_extension("rls.bak");safe(&bak)?;
                if let Ok(v)=fs::read(bak).map_err(|e|e.to_string()).and_then(|v|Snapshot::decode(&v)){Ok((v,true))}
                else if !path.exists()&&!path.with_extension("rls.bak").exists(){Ok((Snapshot::default(),false))} else {Err(original)}
            }
        }
    }
    pub fn slots(&self)->[Slot;SAVE_SLOTS]{std::array::from_fn(|n| match self.read(n){
        Ok((v,recovered))=>Slot{time:v.time,count:v.objects.len(),recovered,broken:false},
        Err(_)=>Slot{broken:true,..Default::default()}
    })}
    pub fn select(&mut self,n:usize)->Result<(),String>{
        if n>=SAVE_SLOTS{return Err(t("存档槽位超出范围").into());}
        let (data,recovered)=self.read(n)?;
        self.data=data;self.active=Some(n);self.recovered=recovered;self.error=None;Ok(())
    }
    fn key(&self,name:&str)->String {
        // Ruffle uses host/path/SOName. Normalize each permitted localPath
        // relative to the movie, so moving the ROM/card retains its progress.
        let mut path=self.movie_path.trim_matches('/');let mut depth=0;
        loop {
            let prefix=format!("localhost/{path}/");
            if let Some(tail)=name.strip_prefix(&prefix){return format!("local@{depth}/{tail}");}
            if let Some((parent,_))=path.rsplit_once('/') {path=parent;depth+=1;} else {break;}
        }
        if let Some(tail)=name.strip_prefix("localhost//"){return format!("root/{tail}");}
        name.into()
    }
    pub fn get(&self,name:&str)->Option<Vec<u8>>{self.data.objects.get(&self.key(name)).cloned()}
    fn commit(&self,n:usize,data:&Snapshot)->Result<(),String>{
        let bytes=data.encode();if bytes.len()>LIMIT{return Err(t("存档超过32MB限制").into());}
        Snapshot::decode(&bytes)?;
        let path=self.path(n);
        if let Ok((old,false))=self.read(n){if old.time!=0 {atomic(&path.with_extension("rls.bak"),&old.encode())?;}}
        atomic(&path,&bytes)
    }
    pub fn put(&mut self,name:&str,value:Option<&[u8]>)->bool{
        let Some(n)=self.active else {self.error=Some(t("请先选择存档").into());return false;};
        let key=self.key(name);
        let mut next=self.data.clone();
        match value {Some(v)=>{next.objects.insert(key,v.to_vec());},None=>{next.objects.remove(&key);}}
        if next.objects==self.data.objects{return true;}
        next.time=now();
        match self.commit(n,&next){Ok(())=>{self.data=next;true},Err(e)=>{self.error=Some(e);false}}
    }
    pub fn copy_to(&mut self,n:usize)->Result<(),String>{
        if n>=SAVE_SLOTS{return Err(t("存档槽位超出范围").into());}
        if self.data.objects.is_empty(){return Err(t("尚无游戏内进度，无法复制").into());}
        let mut data=self.data.clone();data.time=now();self.commit(n,&data)
    }
    pub fn remove(&mut self,name:&str){
        if self.put(name,None){self.pending_deletes.remove(name);}else{self.pending_deletes.insert(name.into());}
    }
    pub fn retry_deletes(&mut self){
        for name in self.pending_deletes.clone(){self.remove(&name);}
    }
    pub fn has_data(&self)->bool{!self.data.objects.is_empty()}
}

#[derive(Clone,Copy,Debug,PartialEq)]
pub struct Settings {pub scale:u8,pub quality:u8,pub fps:bool,pub renderer:super::graphics::Backend}
impl Default for Settings {fn default()->Self{Self{scale:0,quality:2,fps:true,renderer:super::graphics::Backend::OpenGlEs}}}
impl Settings {
    pub fn global_path(controls_root:&Path)->PathBuf {
        controls_root.parent().unwrap_or(controls_root).join("audiovisual/global.rav")
    }
    pub fn requested_renderer(&self,app:bool,override_name:Option<&str>)->super::graphics::Backend {
        use super::graphics::Backend;
        if app{Backend::OpenGlEs}else{override_name.and_then(Backend::from_name).unwrap_or(self.renderer)}
    }
    pub fn recover_renderer(&mut self,actual:super::graphics::Backend,override_name:Option<&str>)->bool {
        use super::graphics::Backend;
        if actual!=Backend::Vulkan||override_name.is_some(){return false;}
        self.renderer=Backend::OpenGlEs;
        true
    }
    pub fn load(path:&Path)->Self {
        let Ok(v)=fs::read(path) else{return Self::default()};
        use super::graphics::Backend;
        let (payload,renderer)=if v.len()==17&&&v[..4]==b"RAV1" {(9,Backend::OpenGlEs)}
            else if v.len()==18&&&v[..4]==b"RAV2" {let Some(renderer)=Backend::from_byte(v[9]) else{return Self::default()};(10,renderer)}
            else{return Self::default()};
        if hash(&v[..payload])!=u64::from_le_bytes(v[payload..].try_into().unwrap()) || v[4]>100 || v[5]>1 || v[6]>2 || v[7]>2 || v[8]>1 {return Self::default();}
        // RAV1 defaults to GLES. Obsolete game volume/mute remain ignored.
        Self{scale:v[6],quality:v[7],fps:v[8]!=0,renderer}
    }
    pub fn save(&self,path:&Path)->Result<(),String>{
        let mut v=b"RAV2".to_vec();v.extend([100,0,self.scale,self.quality,u8::from(self.fps),self.renderer as u8]);v.extend(hash(&v).to_le_bytes());atomic(path,&v)
    }
    pub fn change(&mut self,row:usize,d:i32){match row {
        0=>self.scale=(self.scale as i32+d).rem_euclid(3) as u8,
        1=>self.quality=(self.quality as i32+d).rem_euclid(3) as u8,2=>self.fps=!self.fps,_=>{}
    }}
    pub fn rows(&self)->Vec<String>{vec![crate::tr!("画面缩放                         {}", "Scaling                         {}",[t("保持比例"),t("铺满屏幕"),t("原始大小")][self.scale as usize]),
        crate::tr!("Flash 画质                       {}", "Flash quality                    {}",[t("低"),t("中"),t("高")][self.quality as usize]),
        crate::tr!("左上角帧率                     {}", "FPS counter                     {}",if self.fps{t("开")}else{t("关")}),crate::tr!("渲染接口                         {}  ›", "Graphics API                    {}  ›",self.renderer.label()),t("恢复默认画面设置").into(),t("返回").into()]}
}

#[derive(Clone,Copy,Debug,PartialEq,Default)]
pub enum Page {#[default] Home,Saves,Settings,Renderer,Manager,ManagerGames,ManagerGame,ManagerBulk,Confirm,About,Shortcuts,Language}

pub const SHORTCUT_TABS:[&str;3]=["游戏中","菜单操作","按键编辑"];
// Each row gives one goal, the exact input, and its context or result.
pub const SHORTCUTS:[[(&str,&str,&str);5];3]=[
    [
        ("暂停或继续游戏", "短按 M（菜单键）", "单独按下再松开；在子页面按 M 会先返回上一级。"),
        ("切到上一套按键方案", "按住 M，再按 A", "循环切换此游戏的非空方案，跳过全空方案。"),
        ("切到下一套按键方案", "按住 M，再按 B", "跳过全空方案，屏幕提示切换后的方案名称。"),
        ("互换十字键和摇杆", "按住 M，再按 X", "交换四个方向的操作；再按一次或退出游戏后恢复。"),
        ("保持鼠标左键按下", "按住 M，再按十字键 ↓", "用于拖拽；再按一次松开，打开暂停菜单也会松开。"),
    ],
    [
        ("选择菜单项", "十字键 ↑ ↓", "按 A 进入或确认，按 B 返回；主菜单按 B 继续游戏。"),
        ("在主菜单切换按键方案", "选中「按键设置」后按 ← →", "按 A 可编辑当前方案。"),
        ("调整设置", "十字键 ← →", "在「画面设置」或「鼠标设置」中，先选中要调整的项目。"),
        ("调节系统音量", "机身音量 ＋ / －", "游戏中和暂停菜单中都可以调节。"),
        ("调节屏幕亮度", "按住 M，再按音量键", "按音量 ＋ 提高亮度，按音量 － 降低亮度。"),
    ],
    [
        ("修改一个按键的作用", "十字键选中格子，再按 A", "进入动作选择后，用十字键选动作，按 A 确认。"),
        ("编辑另一套按键方案", "按 L1 或 R1 肩键", "在按键格子页面使用；有未保存修改时会先询问。"),
        ("切换动作分类", "按 L1 或 R1 肩键", "在动作选择页面使用，例如切到键盘、鼠标等分类。"),
        ("给选中的按键开启连发", "在按键格子上按 Y", "每按一次切换：关闭 → 每秒 5 次 → 10 次 → 15 次 → 关闭。"),
        ("保存修改", "选中底部「保存」，再按 A", "保存后，重新打开游戏仍会使用这套设置。"),
    ],
];
pub const SHORTCUT_NOTES:[&str;3]=[
    "组合键：先按住 M，再按另一个键，最后松开；按住不会连续切换。",
    "这里的方向键指机身十字键；L1、R1 指左右肩键。",
    "连发：按住生效，松开停止；支持键盘、组合键和鼠标左右键。",
];
pub const BACKGROUND:[(&str,&str,&str);3]=[
    ("再玩一局熟悉的 Flash", "把熟悉的 Flash 小游戏装进口袋，随时打开，再玩一局。", "Ruffle Flash 基于 Ruffle，为迷你龙 Pocket 1 / LeafOS 适配。"),
    ("项目主页 · GitHub", "https://github.com/changdy/ruffle-leafos", "源码、更新和使用说明都在这里，也欢迎反馈问题、分享建议。"),
    ("感谢开源社区，也感谢你", "感谢 Ruffle、aweigit/ruffle-miyooflip 和 Leaf / Jawaka 的开发者。", "也感谢每一位尝试、测试和反馈的玩家。祝你玩得开心！"),
];
#[derive(Clone,Debug)]
pub enum Command {None,Choose(usize),Copy(usize),Save,Settings,RefreshGames,Renderer(super::graphics::Backend),Clear(usize),Language(super::i18n::Language),Quit}
pub use super::data_manager::Entry;
pub const MANAGER_VISIBLE_ROWS:usize=7;
// Clear is reversible: rename into a dated archive outside active data.
pub fn archive(path:&Path,trash:&Path)->Result<bool,String>{
    safe(path)?;safe(trash)?;
    if !path.exists(){return Ok(false);}
    fs::create_dir_all(trash).map_err(|e|e.to_string())?;
    let target=trash.join(format!("{}-{}",unique(),path.file_name().unwrap_or_default().to_string_lossy()));
    fs::rename(path,&target).map_err(|e|e.to_string())?;
    #[cfg(unix)] {File::open(trash).and_then(|f|f.sync_all()).map_err(|e|e.to_string())?;}
    Ok(true)
}

#[derive(Clone,Debug,Default)]
pub struct Ui {
    pub page:Page,pub selected:usize,pub slots:[Slot;SAVE_SLOTS],pub active:usize,
    pub app:bool,pub started:bool,pub title:String,pub message:String,pub settings:Settings,
    pub games:Vec<Entry>,pub game_index:usize,pub pending:usize,
    pub fps:f64,pub target_fps:f64,
    pub shortcut_tab:usize,
    pub actual_renderer:super::graphics::Backend,
    pub renderer_failed:bool,
    pub language:super::i18n::Language,
}
impl Ui {
    pub fn manager_game(&self)->Option<&Entry>{self.games.get(self.game_index)}
    pub fn open_games(&mut self){self.open(Page::ManagerGames);self.selected=self.game_index.min(self.games.len());}
    pub fn manager_parent(&mut self){
        if self.pending<=2 {self.open(Page::ManagerGame);self.selected=self.pending.saturating_sub(1);}
        else {self.open(Page::ManagerBulk);self.selected=self.pending.saturating_sub(3);}
    }
    pub fn refresh_games(&mut self,games:Vec<Entry>){
        let key=self.manager_game().map(|g|g.key.clone());
        self.games=games;
        let retained=key.and_then(|key|self.games.iter().position(|g|g.key==key));
        self.game_index=retained.unwrap_or(self.game_index.min(self.games.len().saturating_sub(1)));
        if self.page==Page::ManagerGame&&retained.is_none(){self.open_games();}
        else if self.page==Page::ManagerGames{self.selected=self.game_index.min(self.games.len());}
    }
    pub fn manager_visible_start(&self)->usize {self.selected/MANAGER_VISIBLE_ROWS*MANAGER_VISIBLE_ROWS}
    pub fn open(&mut self,page:Page){self.page=page;self.selected=0;self.message.clear();if page==Page::Shortcuts{self.shortcut_tab=0;}if page==Page::Language{self.selected=usize::from(self.language==super::i18n::Language::English);}}
    pub fn heading(&self)->String{match self.page{
        Page::Home=>self.title.clone(),Page::Language=>t("语言").into(),Page::Saves=>t("选择游戏存档").into(),Page::Settings=>t("画面设置").into(),Page::Renderer=>t("渲染接口").into(),Page::Manager=>t("Flash 数据管理").into(),Page::ManagerGames|Page::ManagerGame=>t("游戏数据").into(),Page::ManagerBulk=>t("批量清理").into(),Page::Confirm=>t("确认操作").into(),Page::About=>t("Ruffle Flash · LeafOS 版").into(),Page::Shortcuts=>t("操作说明").into()
    }}
    pub fn rows(&self)->Vec<String>{match self.page{
        Page::Language=>super::i18n::Language::ALL.iter().map(|language| format!("{}{}",language.label(),if *language==self.language{"  ✓"}else{""})).chain(std::iter::once(t("返回").into())).collect(),
        Page::Home=>vec![], Page::Saves=>(0..SAVE_SLOTS).map(|i|crate::tr!("存档 {}{}    {}", "Slot {}{}    {}",i+1,if self.started&&i==self.active {t(" · 当前")}else{""},self.slots[i].detail())).chain(std::iter::once(if self.started{t("返回").into()}else{t("返回 Leaf").into()})).collect(),
        Page::Settings=>self.settings.rows(),
        Page::Renderer=>vec![format!("OpenGL ES                         {}",if self.actual_renderer==super::graphics::Backend::OpenGlEs{t("使用中")}else{""}),
            crate::tr!("Vulkan（实验）                   {}", "Vulkan (experimental)           {}",if self.actual_renderer==super::graphics::Backend::Vulkan{t("使用中")}else if self.renderer_failed{t("上次启动失败")}else{""}),t("返回").into()],
        Page::Manager=>vec![t("游戏数据").into(),t("批量清理").into(),t("语言").into(),t("返回 Leaf").into()],
        Page::ManagerGames=>self.games.iter().map(|g|g.title.clone()).chain(std::iter::once(t("返回").into())).collect(),
        Page::ManagerGame=>vec![t("清空此游戏的全部存档").into(),t("重置此游戏的按键配置").into(),t("返回游戏列表").into()],
        Page::ManagerBulk=>vec![t("清空所有游戏存档").into(),t("重置所有按键配置").into(),t("返回").into()],
        Page::Confirm=>vec![t("取消").into(),t("确认").into()],Page::About|Page::Shortcuts=>vec![]
    }}
    pub fn nav(&mut self,nav:super::controls::Nav)->Command{
        use super::controls::Nav;
        if matches!(self.page,Page::About|Page::Shortcuts) {
            match nav {
                Nav::Left|Nav::Prev if self.page==Page::Shortcuts=>self.shortcut_tab=(self.shortcut_tab+SHORTCUT_TABS.len()-1)%SHORTCUT_TABS.len(),
                Nav::Right|Nav::Next if self.page==Page::Shortcuts=>self.shortcut_tab=(self.shortcut_tab+1)%SHORTCUT_TABS.len(),
                Nav::A|Nav::B=>self.open(Page::Home),_=>{}
            }
            return Command::None;
        }
        let count=self.rows().len();
        match nav {
            Nav::Up=>self.selected=(self.selected+count-1)%count,
            Nav::Down=>self.selected=(self.selected+1)%count,
            Nav::B=>{if self.page==Page::ManagerGames{self.open(Page::Manager);}
                else if self.page==Page::ManagerGame{self.open_games();}
                else if self.page==Page::ManagerBulk{self.open(Page::Manager);self.selected=1;}
                else if self.page==Page::Language{self.open(if self.app{Page::Manager}else{Page::Home});if self.app{self.selected=2;}}
                else if self.page==Page::Renderer{self.open(Page::Settings);self.selected=3;}
                else if self.page==Page::Confirm{if self.app&&self.pending<10{self.manager_parent();}else{self.open(if self.pending>=30{Page::Renderer}else{Page::Saves});}}
                else if self.app||!self.started{return Command::Quit;}else{self.open(Page::Home);}},
            Nav::Prev|Nav::Next if self.page==Page::ManagerGames=>{
                self.selected=if matches!(nav,Nav::Prev){self.selected.saturating_sub(MANAGER_VISIBLE_ROWS)}else{(self.selected+MANAGER_VISIBLE_ROWS).min(count-1)};
            },
            Nav::Left|Nav::Right=>{
                let d=if matches!(nav,Nav::Left){-1}else{1};
                if self.page==Page::Settings&&self.selected<3{self.settings.change(self.selected,d);return Command::Settings;}
            },
            Nav::Y if self.page==Page::Saves&&self.started&&self.selected<SAVE_SLOTS=>{
                if self.selected==self.active{return Command::Save;}
                self.pending=20+self.selected;self.open(Page::Confirm);
            },
            Nav::A=>match self.page{
                Page::Language=>{if let Some(language)=super::i18n::Language::ALL.get(self.selected){return Command::Language(*language);}self.open(if self.app{Page::Manager}else{Page::Home});if self.app{self.selected=2;}},
                Page::Saves=>{if self.selected==SAVE_SLOTS {if self.started{self.open(Page::Home)}else{return Command::Quit;}}
                    else if self.slots[self.selected].broken {self.message=t("此槽位损坏；请先在数据管理中清理").into();}
                    else if !self.started{return Command::Choose(self.selected);}
                    else if self.selected==self.active {self.open(Page::Home);}
                    else {self.pending=10+self.selected;self.open(Page::Confirm);}},
                Page::Settings=>{if self.selected==5{self.open(Page::Home);}else if self.selected==3{self.open(Page::Renderer);}else{if self.selected==4 {self.settings=Settings{renderer:self.settings.renderer,..Settings::default()}}else{self.settings.change(self.selected,1)}return Command::Settings;}},
                Page::Renderer=>{match self.selected {
                    0|1=>{let choice=super::graphics::Backend::from_byte(self.selected as u8).unwrap();
                        if choice==self.actual_renderer{self.message=crate::tr!("当前已使用 {}", "Already using {}",choice.label());}
                        else{self.pending=30+self.selected;self.open(Page::Confirm);}},
                    _=>{self.open(Page::Settings);self.selected=3;}
                }},
                Page::Manager=>match self.selected{0=>{self.open_games();return Command::RefreshGames;},1=>self.open(Page::ManagerBulk),2=>self.open(Page::Language),_=>return Command::Quit},
                Page::ManagerGames=>{if self.selected==self.games.len(){self.open(Page::Manager);}else{self.game_index=self.selected;self.open(Page::ManagerGame);}},
                Page::ManagerGame=>{
                    if self.selected==2{self.open_games();}
                    else if self.manager_game().is_some_and(|g|if self.selected==0{g.has_saves}else{g.has_controls}){self.pending=self.selected+1;self.open(Page::Confirm);}
                    else{self.message=t("没有需要清理的数据").into();}
                },
                Page::ManagerBulk=>{if self.selected==2{self.open(Page::Manager);self.selected=1;}else{self.pending=self.selected+3;self.open(Page::Confirm);}},
                Page::Confirm=>{if self.selected==1{
                    if self.pending>=30{return Command::Renderer(super::graphics::Backend::from_byte((self.pending-30) as u8).unwrap_or_default());}
                    if self.pending>=20{return Command::Copy(self.pending-20);}
                    if self.pending>=10{return Command::Choose(self.pending-10);}
                    return Command::Clear(self.pending);
                } if self.app&&self.pending<10{self.manager_parent();}else{self.open(if self.pending>=30{Page::Renderer}else{Page::Saves});}},
                _=>{}
            },_=>{}
        }
        if self.page==Page::ManagerGames&&self.selected<self.games.len(){self.game_index=self.selected;}
        Command::None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn progress()->Progress {let root=std::env::temp_dir().join(format!("ruffle-progress-{}",unique()));Progress::new(root,b"fixture","测试游戏".into(),"1234".into(),"/mnt/sdcard/Roms/flash/game.swf".into())}
    #[test] fn profiles_survive_restart_are_isolated_and_move_with_rom(){
        let mut p=progress();p.register().unwrap();
        for n in 0..5{p.select(n).unwrap();assert!(p.put("localhost/mnt/sdcard/Roms/flash/game.swf/state",Some(&[n as u8])));}
        let mut moved=Progress::new(p.root.clone(),b"fixture","改名".into(),"1234".into(),"/mnt/other/Roms/flash/renamed.swf".into());
        for n in 0..5{moved.select(n).unwrap();assert_eq!(moved.get("localhost/mnt/other/Roms/flash/renamed.swf/state"),Some(vec![n as u8]));}
        assert!(moved.select(5).is_err());assert_eq!(p.slots().iter().filter(|s|s.time!=0).count(),5);
        fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn torn_primary_recovers_backup_and_never_overwrites_it_with_corruption(){
        let mut p=progress();p.select(0).unwrap();assert!(p.put("name",Some(b"old")));assert!(p.put("name",Some(b"new")));
        fs::write(p.path(0),b"torn").unwrap();p.select(0).unwrap();assert!(p.recovered);assert_eq!(p.get("name"),Some(b"old".to_vec()));
        assert!(p.put("name",Some(b"after-recovery")));fs::write(p.path(0),b"torn-again").unwrap();p.select(0).unwrap();assert_eq!(p.get("name"),Some(b"old".to_vec()));
        fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn missing_primary_with_corrupt_backup_is_not_an_empty_slot(){
        let p=progress();fs::create_dir_all(p.directory()).unwrap();fs::write(p.path(0).with_extension("rls.bak"),b"bad").unwrap();
        assert!(p.slots()[0].broken);fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn failed_write_reports_error_and_keeps_live_data(){
        let mut p=progress();p.select(0).unwrap();assert!(p.put("x",Some(b"old")));
        let old=p.root.clone();p.root=p.path(0);assert!(!p.put("x",Some(b"new")));assert!(p.error.is_some());assert_eq!(p.get("x"),Some(b"old".to_vec()));fs::remove_dir_all(old).unwrap();
    }
    #[test] fn settings_roundtrip_invalid_input_and_defaults(){
        let p=progress();let path=p.root.join("audio");let mut s=Settings::default();assert!(s.fps);assert!(Settings::load(&path).fps);
        s.change(0,1);s.change(1,1);s.change(2,1);assert!(!s.fps);s.save(&path).unwrap();assert_eq!(Settings::load(&path),s);
        let mut legacy=b"RAV1".to_vec();legacy.extend([0,1,s.scale,s.quality,u8::from(s.fps)]);legacy.extend(hash(&legacy).to_le_bytes());fs::write(&path,&legacy).unwrap();
        assert_eq!(Settings::load(&path),s);assert_eq!(fs::read(&path).unwrap(),legacy);
        fs::write(&path,b"corrupt").unwrap();assert_eq!(Settings::load(&path),Settings::default());fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn display_settings_are_shared_across_games_and_ignore_old_game_files(){
        use super::super::{controls::Store,graphics::Backend};
        let p=progress();let root=p.root.join("config/controls");
        let first=Store::new(root.clone(),Path::new("/mnt/sdcard/Roms/flash/first.swf"));
        let second=Store::new(root,Path::new("/mnt/sdcard/Roms/flash/second.swf"));
        assert_ne!(first.identity,second.identity);
        let path=Settings::global_path(&first.root);assert_eq!(path,Settings::global_path(&second.root));
        assert_eq!(path,p.root.join("config/audiovisual/global.rav"));
        let old=path.parent().unwrap().join(format!("{}.rav",game_id(b"first movie")));
        let setting=Settings{scale:2,quality:0,fps:false,renderer:Backend::Vulkan};setting.save(&old).unwrap();
        let old_bytes=fs::read(&old).unwrap();
        assert_eq!(Settings::load(&path),Settings::default());assert!(!path.exists());
        setting.save(&path).unwrap();assert_eq!(Settings::load(&Settings::global_path(&second.root)),setting);
        let mut loaded=Settings::load(&Settings::global_path(&second.root));loaded.change(0,1);loaded.save(&path).unwrap();
        assert_eq!(Settings::load(&Settings::global_path(&first.root)),loaded);
        assert_eq!(fs::read(&old).unwrap(),old_bytes);
        fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn temporary_renderer_choices_do_not_replace_the_saved_global_preference(){
        use super::super::graphics::Backend;
        let p=progress();let path=Settings::global_path(&p.root.join("controls"));
        for preferred in [Backend::OpenGlEs,Backend::Vulkan] {
            let mut settings=Settings{renderer:preferred,..Settings::default()};
            assert_eq!(settings.requested_renderer(false,None),preferred);
            assert_eq!(settings.requested_renderer(true,Some("vulkan")),Backend::OpenGlEs);
            assert_eq!(settings.requested_renderer(false,Some("opengl-es")),Backend::OpenGlEs);
            assert_eq!(settings.requested_renderer(false,Some("vulkan")),Backend::Vulkan);
            settings.change(0,1);settings.save(&path).unwrap();
            assert_eq!(Settings::load(&path).renderer,preferred);
        }
        fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn failed_clear_is_retried_before_a_successful_flush(){
        let mut p=progress();p.select(0).unwrap();assert!(p.put("x",Some(b"old")));let root=p.root.clone();
        p.root=p.path(0);p.remove("x");assert!(p.error.is_some());assert_eq!(p.get("x"),Some(b"old".to_vec()));
        p.root=root;p.error=None;p.retry_deletes();assert!(p.error.is_none());p.select(0).unwrap();assert_eq!(p.get("x"),None);fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn copy_and_clear_are_scoped_and_recoverable(){
        let mut p=progress();p.register().unwrap();p.select(0).unwrap();assert!(p.copy_to(1).is_err());assert!(p.put("save",Some(b"progress")));p.copy_to(4).unwrap();p.select(4).unwrap();assert_eq!(p.get("save"),Some(b"progress".to_vec()));
        let controls=p.root.join("controls");
        assert_eq!(super::super::data_manager::entries(&p.root,&controls,&[]).len(),1);let trash=p.root.join("trash");assert!(archive(&p.directory(),&trash).unwrap());assert!(super::super::data_manager::entries(&p.root,&controls,&[]).is_empty());assert!(p.slots().iter().all(|s|s.time==0));assert_eq!(fs::read_dir(trash).unwrap().count(),1);fs::remove_dir_all(p.root).unwrap();
    }
    fn manager()->Ui {
        let mut u=Ui{app:true,..Ui::default()};
        u.games=(0..22).map(|i|Entry{key:format!("game-{i}"),title:format!("Game {i}"),has_saves:true,has_controls:true,..Entry::default()}).collect();
        u.open(Page::Manager);u
    }
    #[test] fn manager_list_pages_and_back_preserve_game_and_confirm_context(){
        use super::super::controls::Nav;
        let mut u=manager();assert!(matches!(u.nav(Nav::A),Command::RefreshGames));assert_eq!(u.page,Page::ManagerGames);
        u.nav(Nav::Next);assert_eq!(u.selected,7);assert_eq!(u.manager_visible_start(),7);
        u.nav(Nav::Next);assert_eq!(u.selected,14);u.nav(Nav::A);assert_eq!(u.game_index,14);assert_eq!(u.page,Page::ManagerGame);
        u.nav(Nav::A);assert_eq!(u.page,Page::Confirm);assert_eq!(u.selected,0);u.nav(Nav::A);assert_eq!(u.page,Page::ManagerGame);
        u.nav(Nav::Down);u.nav(Nav::A);assert_eq!(u.pending,2);u.nav(Nav::B);assert_eq!(u.selected,1);assert_eq!(u.page,Page::ManagerGame);
        u.nav(Nav::B);assert_eq!(u.page,Page::ManagerGames);assert_eq!(u.selected,14);
        u.nav(Nav::Prev);assert_eq!(u.selected,7);u.nav(Nav::B);assert_eq!(u.page,Page::Manager);
        u.nav(Nav::A);assert_eq!(u.selected,7);u.nav(Nav::B);
        u.selected=1;u.nav(Nav::A);assert_eq!(u.page,Page::ManagerBulk);u.selected=1;u.nav(Nav::A);assert_eq!(u.pending,4);assert_eq!(u.selected,0);
        u.nav(Nav::B);assert_eq!(u.page,Page::ManagerBulk);assert_eq!(u.selected,1);u.nav(Nav::A);u.selected=1;assert!(matches!(u.nav(Nav::A),Command::Clear(4)));
    }
    #[test] fn manager_refresh_removes_empty_game_and_retains_remaining_data(){
        use super::super::controls::Nav;
        let mut u=manager();u.game_index=14;u.open(Page::ManagerGame);
        let mut games=u.games.clone();games[14].has_saves=false;u.refresh_games(games);assert_eq!(u.page,Page::ManagerGame);assert_eq!(u.game_index,14);
        u.nav(Nav::A);assert_eq!(u.page,Page::ManagerGame);assert!(!u.message.is_empty());
        let mut games=u.games.clone();games.remove(14);u.refresh_games(games);assert_eq!(u.page,Page::ManagerGames);assert_eq!(u.selected,14);
        u.refresh_games(vec![]);assert_eq!(u.selected,0);u.nav(Nav::Next);u.nav(Nav::Prev);assert_eq!(u.selected,0);
        u.nav(Nav::A);assert_eq!(u.page,Page::Manager);assert!(matches!(u.nav(Nav::B),Command::Quit));
    }
    #[test] fn manager_labels_translate_without_translating_game_names(){
        use super::super::i18n::{self,Language};
        let mut u=manager();u.games[0].title="中文游戏".into();
        i18n::set(Language::English);assert_eq!(u.rows()[0],"Game data");assert_eq!(t("（已失效）"),"(Unavailable)");
        u.open_games();assert_eq!(u.rows()[0],"中文游戏");u.open(Page::ManagerBulk);assert_eq!(u.heading(),"Bulk cleanup");
        i18n::set(Language::Chinese);
    }
    #[test] fn startup_cannot_bypass_slot_picker_and_overwrite_requires_confirmation(){
        use super::super::controls::Nav;
        let mut u=Ui::default();u.open(Page::Saves);assert!(matches!(u.nav(Nav::B),Command::Quit));
        u.started=true;u.active=0;u.open(Page::Saves);u.selected=1;assert!(matches!(u.nav(Nav::A),Command::None));assert_eq!(u.page,Page::Confirm);assert_eq!(u.selected,0);
        assert!(matches!(u.nav(Nav::A),Command::None));assert_eq!(u.page,Page::Saves);
        u.selected=1;u.nav(Nav::Y);assert_eq!(u.page,Page::Confirm);u.selected=1;assert!(matches!(u.nav(Nav::A),Command::Copy(1)));
    }
    #[test] fn shortcut_pages_wrap_reset_and_return_without_changing_settings_or_progress(){
        use super::super::controls::Nav;
        let mut u=Ui::default();u.started=true;u.active=3;u.settings.scale=2;let original=u.settings;
        u.open(Page::Shortcuts);assert_eq!(u.shortcut_tab,0);
        u.nav(Nav::Left);assert_eq!(u.shortcut_tab,2);
        u.nav(Nav::Next);assert_eq!(u.shortcut_tab,0);
        u.nav(Nav::Prev);assert_eq!(u.shortcut_tab,2);
        u.nav(Nav::Right);assert_eq!(u.shortcut_tab,0);
        for tab in [1,2,0] {assert!(matches!(u.nav(Nav::Right),Command::None));assert_eq!(u.shortcut_tab,tab);}
        u.nav(Nav::Next);u.nav(Nav::Up);u.nav(Nav::Down);assert_eq!(u.shortcut_tab,1);
        assert!(matches!(u.nav(Nav::B),Command::None));assert_eq!(u.page,Page::Home);
        u.open(Page::Shortcuts);assert_eq!(u.shortcut_tab,0);u.nav(Nav::A);assert_eq!(u.page,Page::Home);
        assert_eq!(u.active,3);assert_eq!(u.settings,original);
    }
    #[test] fn about_is_a_single_page_and_only_confirm_or_back_leaves_it(){
        use super::super::controls::Nav;
        let mut u=Ui::default();u.started=true;u.active=3;u.settings.scale=2;let original=u.settings;
        for exit in [Nav::A,Nav::B] {
            u.open(Page::About);
            for nav in [Nav::Left,Nav::Right,Nav::Prev,Nav::Next,Nav::Up,Nav::Down,Nav::Y] {
                assert!(matches!(u.nav(nav),Command::None));assert_eq!(u.page,Page::About);
            }
            u.nav(exit);assert_eq!(u.page,Page::Home);
        }
        assert_eq!(u.active,3);assert_eq!(u.settings,original);
    }
    #[test] fn renderer_overrides_and_runtime_recovery_preserve_saved_settings(){
        use super::super::graphics::Backend;
        let p=progress();let path=p.root.join("renderer-preference");
        for preferred in [Backend::OpenGlEs,Backend::Vulkan] {
            for app in [false,true] {
                for override_name in [None,Some("opengl-es"),Some("vulkan")] {
                    let mut settings=Settings{scale:2,quality:0,fps:false,renderer:preferred};
                    let actual=settings.requested_renderer(app,override_name);
                    let expected=if app{Backend::OpenGlEs}else{override_name.and_then(Backend::from_name).unwrap_or(preferred)};
                    assert_eq!(actual,expected);assert_eq!(settings.renderer,preferred);
                    settings.change(2,1);settings.save(&path).unwrap();
                    assert_eq!(Settings::load(&path),settings);
                    let mut recovered=settings;
                    let should_recover=actual==Backend::Vulkan&&override_name.is_none();
                    assert_eq!(recovered.recover_renderer(actual,override_name),should_recover);
                    assert_eq!(recovered,Settings{renderer:if should_recover{Backend::OpenGlEs}else{preferred},..settings});
                    assert_eq!(Settings::load(&path),settings,"runtime selection must not write preferences");
                }
            }
        }
        fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn calendar_is_explicit_china_time(){assert_eq!(date(0),"1970-01-01 08:00:00");assert_eq!(date(1709164800),"2024-02-29 08:00:00");}
    #[test] fn renderer_page_is_reachable_and_back_preserves_settings_and_progress() {
        use super::super::controls::Nav;
        let mut u=Ui::default();u.started=true;u.active=3;u.settings.scale=2;let original=u.settings;
        u.open(Page::Settings);u.selected=3;
        assert!(matches!(u.nav(Nav::A),Command::None));assert_eq!(u.page,Page::Renderer);
        u.nav(Nav::A);assert_eq!(u.page,Page::Renderer);assert!(u.message.contains("OpenGL ES"));
        u.nav(Nav::B);assert_eq!(u.page,Page::Settings);assert_eq!(u.selected,3);
        u.nav(Nav::A);u.selected=2;u.nav(Nav::A);assert_eq!(u.page,Page::Settings);assert_eq!(u.selected,3);
        assert_eq!(u.settings,original);assert_eq!(u.active,3);
        u.selected=5;u.nav(Nav::A);assert_eq!(u.page,Page::Home);
    }
    #[test] fn renderer_switch_in_both_directions_requires_confirmation_and_preserves_live_state() {
        use super::super::{controls::Nav,graphics::Backend};
        for (active,choice) in [(Backend::OpenGlEs,Backend::Vulkan),(Backend::Vulkan,Backend::OpenGlEs)] {
            let mut u=Ui::default();u.started=true;u.active=2;u.settings.fps=true;u.settings.renderer=active;u.actual_renderer=active;let original=u.settings;
            u.open(Page::Renderer);u.selected=choice as usize;
            assert!(matches!(u.nav(Nav::A),Command::None));assert_eq!(u.page,Page::Confirm);assert_eq!(u.selected,0);
            assert!(matches!(u.nav(Nav::A),Command::None));assert_eq!(u.page,Page::Renderer);
            u.selected=choice as usize;u.nav(Nav::A);u.nav(Nav::B);assert_eq!(u.page,Page::Renderer);
            u.selected=choice as usize;u.nav(Nav::A);u.selected=1;
            assert!(matches!(u.nav(Nav::A),Command::Renderer(backend) if backend==choice));
            assert_eq!(u.settings,original);assert_eq!(u.actual_renderer,active);assert_eq!(u.active,2);
        }
    }
    #[test] fn renderer_preference_reads_legacy_without_rewriting_and_rejects_invalid_rav2() {
        use super::super::graphics::Backend;
        let p=progress();let path=p.root.join("graphics");let mut s=Settings::default();s.scale=2;s.renderer=Backend::Vulkan;
        s.save(&path).unwrap();let encoded=fs::read(&path).unwrap();assert_eq!(encoded.len(),18);assert_eq!(Settings::load(&path),s);
        let mut invalid=encoded.clone();invalid[9]=2;let sum=hash(&invalid[..10]);invalid[10..].copy_from_slice(&sum.to_le_bytes());
        fs::write(&path,&invalid).unwrap();assert_eq!(Settings::load(&path),Settings::default());assert_eq!(fs::read(&path).unwrap(),invalid);
        for len in 0..encoded.len(){fs::write(&path,&encoded[..len]).unwrap();assert_eq!(Settings::load(&path),Settings::default());}
        let mut legacy=b"RAV1".to_vec();legacy.extend([100,0,2,1,1]);legacy.extend(hash(&legacy).to_le_bytes());fs::write(&path,&legacy).unwrap();
        let loaded=Settings::load(&path);assert_eq!((loaded.scale,loaded.quality,loaded.fps,loaded.renderer),(2,1,true,Backend::OpenGlEs));assert_eq!(fs::read(&path).unwrap(),legacy);
        fs::remove_dir_all(p.root).unwrap();
    }
    #[test] fn settings_reset_and_navigation_still_work_with_renderer_entry() {
        use super::super::controls::Nav;
        let mut u=Ui::default();u.started=true;u.settings.scale=2;u.settings.fps=false;u.open(Page::Settings);
        u.nav(Nav::Up);assert_eq!(u.selected,5);u.nav(Nav::Down);assert_eq!(u.selected,0);
        u.settings.renderer=super::super::graphics::Backend::Vulkan;
        u.selected=4;assert!(matches!(u.nav(Nav::A),Command::Settings));assert_eq!(u.settings,Settings{renderer:super::super::graphics::Backend::Vulkan,..Settings::default()});
        u.selected=3;let original=u.settings;
        for nav in [Nav::Left,Nav::Right] {assert!(matches!(u.nav(nav),Command::None));}
        assert_eq!(u.settings,original);assert_eq!(u.page,Page::Settings);
    }
}
