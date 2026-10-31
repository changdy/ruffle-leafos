//! Leaf artwork naming, mounted-card selection and screenshot file writes.
//! No SDL/GL dependencies: filesystem and overwrite behavior run in host tests.
use std::{fs::{self, OpenOptions}, io::{self, Write}, path::{Path, PathBuf}};
#[cfg(unix)] use std::fs::File;

#[derive(Clone, Debug)]
pub struct Cover {
    pub target: PathBuf,
    pub existing: Option<PathBuf>,
}

fn image(dir: &Path, stem: &std::ffi::OsStr) -> io::Result<Option<PathBuf>> {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e),
    };
    let mut found = Vec::new();
    for entry in entries {
        let path = entry?.path();
        let ext = path.extension().and_then(|s| s.to_str()).unwrap_or("");
        let format = match ext.to_ascii_lowercase().as_str() { "png" => 0, "jpg" => 1, "jpeg" => 2, _ => continue };
        // Looking up the requested stem with the candidate's actual extension
        // also follows the filesystem's case rules (FAT versus ext4).
        let mut name=stem.to_os_string();name.push(".");name.push(ext);
        let candidate = dir.join(name);
        if candidate.is_file() && path.is_file() && same_file(&candidate, &path) {
            let spelling = if ext == ext.to_ascii_lowercase() {0} else if ext == ext.to_ascii_uppercase() {1} else {2};
            found.push(((format, spelling, path.clone()), path));
        }
    }
    found.sort_by(|a,b| a.0.cmp(&b.0));
    Ok(found.into_iter().next().map(|(_,path)| path))
}

fn same_file(a: &Path, b: &Path) -> bool {
    #[cfg(unix)] {
        use std::os::unix::fs::MetadataExt;
        match (fs::metadata(a),fs::metadata(b)) { (Ok(a),Ok(b)) => a.dev()==b.dev() && a.ino()==b.ino(), _ => false }
    }
    #[cfg(not(unix))] {a.to_string_lossy().eq_ignore_ascii_case(&b.to_string_lossy())}
}

/// Leaf's artwork namespace is flat even for games below a ROM subfolder.
pub fn cover(rom: &Path, cards: &[PathBuf]) -> io::Result<Cover> {
    let rom = fs::canonicalize(rom)?;
    let mut owner = None;
    for card in cards {
        let Ok(card) = fs::canonicalize(card) else {continue};
        if rom.starts_with(card.join("Roms/flash")) || rom.starts_with(card.join("Roms/FLASH")) {
            owner = Some(card); break;
        }
    }
    let card = owner.ok_or_else(||io::Error::other("SWF is outside a Leaf Roms/flash folder"))?;
    let mut stem = rom.file_stem().ok_or_else(||io::Error::other("SWF has no file name"))?.to_os_string();
    // Leaf removes a repeated content extension too (game.swf.swf).
    if Path::new(&stem).extension().is_some_and(|s| s.eq_ignore_ascii_case("swf")) {
        stem = Path::new(&stem).file_stem().unwrap().to_os_string();
    }
    let dir = card.join("Images/flash");
    let canonical = image(&dir,&stem)?;
    let existing = match &canonical {Some(path)=>Some(path.clone()), None=>image(&card.join("Roms/flash/Imgs"),&stem)?};
    let target = canonical.filter(|p|p.extension().is_some_and(|e|e.eq_ignore_ascii_case("png")))
        .unwrap_or_else(||{let mut name=stem;name.push(".png");dir.join(name)});
    Ok(Cover {target,existing})
}

pub fn cards() -> Vec<PathBuf> {
    let primary = std::env::var_os("SDCARD_PATH").map(PathBuf::from).unwrap_or_else(||"/mnt/sdcard".into());
    let mut cards = vec![primary];
    if let Some(paths)=std::env::var_os("SDCARD_PATHS") {cards.extend(std::env::split_paths(&paths));}
    cards.push(std::env::var_os("UMRK_SECONDARY_SDCARD_PATH").map(PathBuf::from).unwrap_or_else(||"/media/sdcard1".into()));
    cards.dedup(); cards
}

fn mount_unescape(path: &str) -> String {
    path.replace("\\040"," ").replace("\\011","\t").replace("\\012","\n").replace("\\134","\\")
}

/// An existing empty mount directory, a symlink to the system card, or a
/// bind mount on the same filesystem must never count as a second TF card.
pub fn photo_root(cards: &[PathBuf], mounts: &str) -> io::Result<PathBuf> {
    let primary = fs::canonicalize(cards.first().ok_or_else(||io::Error::other("Missing system card"))?)?;
    let mounted: Vec<_> = mounts.lines().filter_map(|line| {
        let fields: Vec<_> = line.split_whitespace().collect();
        Some((PathBuf::from(mount_unescape(fields.get(4)?)), *fields.get(2)?))
    }).collect();
    let primary_device = mounted.iter().filter(|(path,_)|primary.starts_with(path)).max_by_key(|(path,_)|path.components().count()).map(|(_,dev)|*dev);
    for card in cards.iter().skip(1) {
        let Ok(card)=fs::canonicalize(card) else {continue};
        if card==primary {continue;}
        if mounted.iter().any(|(path,dev)|path==&card && Some(*dev)!=primary_device) {return Ok(card);}
    }
    Ok(primary)
}

pub fn save_photo(root: &Path, title: &str, timestamp: &str, bytes: &[u8]) -> io::Result<PathBuf> {
    let title: String = title.chars().map(|c|if c.is_control() || "<>:\"/\\|?*".contains(c) {'_'}else{c}).take(64).collect();
    let title=title.trim().trim_end_matches('.');
    let dir=root.join("Pictures/Ruffle Flash");fs::create_dir_all(&dir)?;
    for suffix in 0..1000 {
        let path=dir.join(format!("{}-{timestamp}-{suffix:03}.png",if title.is_empty(){"Flash"}else{title}));
        let mut file=match OpenOptions::new().write(true).create_new(true).open(&path) {
            Ok(file)=>file,Err(e) if e.kind()==io::ErrorKind::AlreadyExists=>continue,Err(e)=>return Err(e),
        };
        if let Err(e)=file.write_all(bytes).and_then(|_|file.sync_all()) {drop(file);let _=fs::remove_file(&path);return Err(e);}
        return Ok(path);
    }
    Err(io::Error::new(io::ErrorKind::AlreadyExists,"Too many photos with this timestamp"))
}

pub fn save_cover(cover: &Cover, overwrite: bool, bytes: &[u8]) -> io::Result<()> {
    if !overwrite && (cover.existing.is_some() || cover.target.try_exists()?) {
        return Err(io::Error::new(io::ErrorKind::AlreadyExists,"Cover exists; confirm overwrite"));
    }
    let dir=cover.target.parent().ok_or_else(||io::Error::other("Missing cover folder"))?;
    fs::create_dir_all(dir)?;
    let temp=dir.join(format!(".ruffle-cover-{}-{}.tmp",std::process::id(),super::experience::unique()));
    let result=(|| {
        let mut file=OpenOptions::new().write(true).create_new(true).open(&temp)?;
        file.write_all(bytes)?;file.sync_all()?;drop(file);
        if !overwrite && cover.target.try_exists()? {return Err(io::Error::new(io::ErrorKind::AlreadyExists,"Cover changed; retry"));}
        super::experience::replace(&temp,&cover.target)?;
        #[cfg(unix)] File::open(dir)?.sync_all()?;
        Ok(())
    })();
    if result.is_err() {let _=fs::remove_file(&temp);} result
}

/// Convert a top-left stage rectangle to a clipped, integer pixel rectangle.
pub fn crop(screen: (u32,u32), stage: (f64,f64,f64,f64)) -> Option<(u32,u32,u32,u32)> {
    let (x,y,w,h)=stage;
    if ![x,y,w,h].iter().all(|v|v.is_finite()) || w<=0. || h<=0. {return None;}
    let left=x.ceil().max(0.).min(screen.0 as f64) as u32;
    let top=y.ceil().max(0.).min(screen.1 as f64) as u32;
    let right=(x+w).floor().max(0.).min(screen.0 as f64) as u32;
    let bottom=(y+h).floor().max(0.).min(screen.1 as f64) as u32;
    (right>left && bottom>top).then_some((left,top,right.saturating_sub(left),bottom.saturating_sub(top)))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture()->PathBuf {let p=std::env::temp_dir().join(format!("ruffle-capture-{}",super::super::experience::unique()));fs::create_dir_all(&p).unwrap();p}
    #[test] fn artwork_is_flat_unicode_and_prompts_for_legacy_jpeg() {
        let card=fixture();let rom=card.join("Roms/flash/子目录/游戏.swf");fs::create_dir_all(rom.parent().unwrap()).unwrap();fs::write(&rom,b"swf").unwrap();
        let legacy=card.join("Roms/flash/Imgs/游戏.JpG");fs::create_dir_all(legacy.parent().unwrap()).unwrap();fs::write(&legacy,b"old").unwrap();
        let plan=cover(&rom,&[card.clone()]).unwrap();assert_eq!(plan.target,fs::canonicalize(&card).unwrap().join("Images/flash/游戏.png"));assert_eq!(plan.existing,Some(fs::canonicalize(&legacy).unwrap()));
        assert_eq!(save_cover(&plan,false,b"new").unwrap_err().kind(),io::ErrorKind::AlreadyExists);assert!(!plan.target.exists());
        save_cover(&plan,true,b"new").unwrap();assert_eq!(fs::read(&legacy).unwrap(),b"old");assert_eq!(fs::read(&plan.target).unwrap(),b"new");
        assert!(cover(&rom,&[card.clone()]).unwrap().existing.unwrap().ends_with("Images/flash/游戏.png"));fs::remove_dir_all(card).unwrap();
    }
    #[test] fn photos_do_not_overwrite_or_escape_and_offline_card_is_not_selected() {
        let root=fixture();let primary=root.join("system");let second=root.join("second");fs::create_dir_all(&primary).unwrap();fs::create_dir_all(&second).unwrap();
        let cards=vec![primary.clone(),second.clone()];let mounts=format!("1 0 8:1 / {} rw - vfat /dev/one rw\n",fs::canonicalize(&primary).unwrap().display());
        assert_eq!(photo_root(&cards,&mounts).unwrap(),fs::canonicalize(&primary).unwrap());
        let mounts=format!("{mounts}2 0 8:2 / {} rw - vfat /dev/two rw\n",fs::canonicalize(&second).unwrap().display());assert_eq!(photo_root(&cards,&mounts).unwrap(),fs::canonicalize(&second).unwrap());
        let same=mounts.replace("8:2","8:1");assert_eq!(photo_root(&cards,&same).unwrap(),fs::canonicalize(&primary).unwrap());
        let first=save_photo(&primary,"../游戏:名字","20261031-101112",b"one").unwrap();let next=save_photo(&primary,"../游戏:名字","20261031-101112",b"two").unwrap();assert_ne!(first,next);assert_eq!(fs::read(first).unwrap(),b"one");assert_eq!(fs::read(next).unwrap(),b"two");fs::remove_dir_all(root).unwrap();
    }
    #[test] fn crop_excludes_letterbox_and_clips_large_original_stage() {
        assert_eq!(crop((960,720),(0.,90.,960.,540.)),Some((0,90,960,540)));
        assert_eq!(crop((960,720),(-160.,-40.,1280.,800.)),Some((0,0,960,720)));
        assert_eq!(crop((960,720),(80.,60.,800.,600.)),Some((80,60,800,600)));
        assert_eq!(crop((960,720),(1000.,0.,20.,20.)),None);
    }
}
