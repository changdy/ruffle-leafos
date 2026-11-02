//! Extract directory games to an app-owned, content-addressed cache.
use sha2::{Digest, Sha256};
use std::{collections::HashSet, fs::{self, File}, io::{Read, Seek, SeekFrom, Write}, path::{Component, Path, PathBuf}, sync::atomic::{AtomicU64,Ordering}};
use zip::ZipArchive;

const MAX_FILES: usize = 10_000;
const MAX_BYTES: u64 = 4 * 1024 * 1024 * 1024;
static SERIAL: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Debug)]
pub struct Progress { pub percent:u8 }

fn safe_name(name:&str)->Result<PathBuf,String> {
    if name.is_empty() || name.contains('\\') || name.contains(':') || name.chars().any(char::is_control) {
        return Err(format!("Invalid ZIP path: {name:?}"));
    }
    let path=Path::new(name.trim_end_matches('/'));
    if path.as_os_str().is_empty() || !path.components().all(|c|matches!(c,Component::Normal(_))) {
        return Err(format!("Unsafe ZIP path: {name:?}"));
    }
    Ok(path.to_owned())
}

fn valid_cache(root:&Path,main:&Path,entries:&[(PathBuf,u64,u32)])->bool {
    if !fs::symlink_metadata(root).is_ok_and(|m|m.is_dir() && !m.file_type().is_symlink()) {return false;}
    if fs::read(root.join(".complete")).ok().as_deref()!=Some(main.to_string_lossy().as_bytes()) {return false;}
    entries.iter().all(|(relative,size,crc)| {
        let mut current=root.to_owned();
        for component in relative.components() {
            current.push(component.as_os_str());
            if fs::symlink_metadata(&current).map_or(true,|m|m.file_type().is_symlink()) {return false;}
        }
        if !fs::metadata(&current).is_ok_and(|m|m.is_file() && m.len()==*size) {return false;}
        if relative!=main {return true;}
        let Ok(mut file)=File::open(current) else {return false};
        let mut checksum=crc32fast::Hasher::new();let mut buffer=[0u8;65536];
        loop {match file.read(&mut buffer) {Ok(0)=>break,Ok(n)=>checksum.update(&buffer[..n]),Err(_)=>return false}}
        checksum.finalize()==*crc
    })
}

pub fn prepare<F:FnMut(Progress)>(zip_path:&Path,cache:&Path,mut report:F)->Result<PathBuf,String> {
    let mut input=File::open(zip_path).map_err(|e|format!("Cannot open ZIP: {e}"))?;
    let zip_len=input.metadata().map_err(|e|e.to_string())?.len();
    let mut hasher=Sha256::new();let mut buffer=[0u8;65536];let mut read=0u64;
    loop {let n=input.read(&mut buffer).map_err(|e|e.to_string())?;if n==0{break;}hasher.update(&buffer[..n]);read+=n as u64;report(Progress{percent:(read.saturating_mul(20)/zip_len.max(1)).min(20) as u8});}
    let key=format!("{:x}",hasher.finalize());
    input.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
    let mut archive=ZipArchive::new(input).map_err(|e|format!("Invalid ZIP: {e}"))?;
    if archive.len()==0 || archive.len()>MAX_FILES {return Err("ZIP has no files or too many entries".into());}
    let mut names=HashSet::new();let mut entries=Vec::new();let mut movies=Vec::new();let mut total=0u64;
    for i in 0..archive.len() {
        let entry=archive.by_index(i).map_err(|e|e.to_string())?;
        let path=safe_name(entry.name())?;
        if path.to_string_lossy().eq_ignore_ascii_case(".complete") {return Err("Reserved ZIP path: .complete".into());}
        if !names.insert(path.to_string_lossy().to_lowercase()) {return Err(format!("Duplicate ZIP path: {}",path.display()));}
        if entry.unix_mode().is_some_and(|m|m & 0o170000==0o120000) {return Err("ZIP symlinks are not supported".into());}
        if entry.is_dir(){continue;}
        total=total.checked_add(entry.size()).ok_or("ZIP size overflow")?;
        if total>MAX_BYTES{return Err("ZIP unpacked size exceeds 4 GiB".into());}
        if path.extension().is_some_and(|x|x.eq_ignore_ascii_case("swf")){movies.push(path.clone());}
        entries.push((path,entry.size(),entry.crc32()));
    }
    if movies.is_empty(){return Err("ZIP does not contain an SWF".into());}
    let depth=movies.iter().map(|p|p.components().count()).min().unwrap();
    let nearest:Vec<_>=movies.iter().filter(|p|p.components().count()==depth).collect();
    let stem=zip_path.file_stem().unwrap_or_default().to_string_lossy();
    let matching:Vec<_>=nearest.iter().filter(|p|p.file_stem().is_some_and(|s|s.to_string_lossy().eq_ignore_ascii_case(&stem))).collect();
    let main=if nearest.len()==1 {nearest[0].clone()} else if matching.len()==1 {(*matching[0]).clone()} else {return Err("ZIP contains multiple possible main SWFs; name the ZIP after the main SWF".into());};
    fs::create_dir_all(cache).map_err(|e|format!("Cannot create ZIP cache: {e}"))?;
    let destination=cache.join(key);
    if valid_cache(&destination,&main,&entries) {report(Progress{percent:100});return Ok(destination.join(main));}
    let suffix=format!("{}-{}",std::process::id(),SERIAL.fetch_add(1,Ordering::Relaxed));
    let stage=cache.join(format!(".extract-{suffix}"));
    fs::create_dir(&stage).map_err(|e|format!("Cannot create ZIP staging directory: {e}"))?;
    let result=(||->Result<(),String>{
        let mut extracted=0u64;
        for i in 0..archive.len() {
            let mut entry=archive.by_index(i).map_err(|e|e.to_string())?;
            if entry.is_dir(){continue;}
            let relative=safe_name(entry.name())?;let out=stage.join(relative);
            fs::create_dir_all(out.parent().ok_or("ZIP entry missing parent")?).map_err(|e|e.to_string())?;
            let mut file=File::create(&out).map_err(|e|format!("Cannot extract {}: {e}",out.display()))?;
            loop {let n=entry.read(&mut buffer).map_err(|e|format!("ZIP read failed: {e}"))?;if n==0{break;}
                file.write_all(&buffer[..n]).map_err(|e|format!("ZIP write failed: {e}"))?;
                extracted+=n as u64;report(Progress{percent:(20+extracted.saturating_mul(80)/total.max(1)).min(100) as u8});
            }
        }
        let mut marker=File::create(stage.join(".complete")).map_err(|e|e.to_string())?;
        marker.write_all(main.to_string_lossy().as_bytes()).map_err(|e|e.to_string())?;
        marker.sync_all().map_err(|e|e.to_string())?;
        drop(marker);
        let old=cache.join(format!(".old-{suffix}"));
        if destination.exists(){fs::rename(&destination,&old).map_err(|e|format!("Cannot replace invalid ZIP cache: {e}"))?;}
        if let Err(e)=fs::rename(&stage,&destination){if old.exists(){let _=fs::rename(&old,&destination);}return Err(e.to_string());}
        if old.exists(){let _=fs::remove_dir_all(old);}
        Ok(())
    })();
    if result.is_err(){let _=fs::remove_dir_all(&stage);}
    result?;
    Ok(destination.join(main))
}

#[cfg(test)]
mod tests {
    use super::*;
    use zip::{write::SimpleFileOptions,ZipWriter};
    struct Fixture(PathBuf);
    impl Fixture {
        fn new()->Self {
            let path=std::env::temp_dir().join(format!("ruffle-zip-test-{}-{}",std::process::id(),SERIAL.fetch_add(1,Ordering::Relaxed)));
            fs::create_dir(&path).unwrap();Self(path)
        }
        fn archive(&self,name:&str,files:&[(&str,&[u8])])->PathBuf {
            let path=self.0.join(name);let mut z=ZipWriter::new(File::create(&path).unwrap());
            for (name,data) in files {z.start_file(*name,SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated)).unwrap();z.write_all(data).unwrap();}
            z.finish().unwrap();path
        }
        fn prepare(&self,path:&Path)->Result<PathBuf,String>{prepare(path,&self.0.join("cache"),|_|{})}
    }
    impl Drop for Fixture {fn drop(&mut self){let _=fs::remove_dir_all(&self.0);}}
    #[test]
    fn archive_extracts_relative_resources_reuses_and_repairs_missing_file() {
        let f=Fixture::new();let z=f.archive("game.zip",&[("game.swf",b"movie"),("assets/data/test.swf",b"resource")]);
        let main=f.prepare(&z).unwrap();let resource=main.parent().unwrap().join("assets/data/test.swf");
        assert_eq!(fs::read(&main).unwrap(),b"movie");assert_eq!(fs::read(&resource).unwrap(),b"resource");
        assert_eq!(f.prepare(&z).unwrap(),main);
        fs::write(&main,b"other").unwrap();assert_eq!(f.prepare(&z).unwrap(),main);
        assert_eq!(fs::read(&main).unwrap(),b"movie");
        fs::remove_file(&resource).unwrap();assert_eq!(f.prepare(&z).unwrap(),main);
        assert_eq!(fs::read(resource).unwrap(),b"resource");
        let z=f.archive("game.zip",&[("game.swf",b"new movie"),("assets/data/test.swf",b"resource")]);
        assert_ne!(f.prepare(&z).unwrap(),main);
    }
    #[test]
    fn main_selection_supports_one_enclosing_folder_and_refuses_ambiguity() {
        let f=Fixture::new();let z=f.archive("game.zip",&[("folder/game.swf",b"movie"),("folder/assets/part.swf",b"resource")]);
        assert!(f.prepare(&z).unwrap().ends_with("folder/game.swf"));
        let z=f.archive("ambiguous.zip",&[("a.swf",b"a"),("b.swf",b"b")]);
        assert!(f.prepare(&z).unwrap_err().contains("multiple possible"));
        let z=f.archive("a.zip",&[("a.swf",b"a"),("b.swf",b"b")]);
        assert!(f.prepare(&z).unwrap().ends_with("a.swf"));
    }
    #[test]
    fn rejects_escape_duplicate_and_reserved_paths() {
        let f=Fixture::new();
        for entries in [vec![("../escape.swf",b"x".as_slice())],vec![("A.swf",b"x".as_slice()),("a.swf",b"y".as_slice())],
                        vec![("game.swf",b"x".as_slice()),(".complete",b"bad".as_slice())],vec![("C:/bad.swf",b"x".as_slice())]] {
            let z=f.archive("bad.zip",&entries);assert!(f.prepare(&z).is_err());
        }
        assert!(!f.0.join("escape.swf").exists());
    }
}
