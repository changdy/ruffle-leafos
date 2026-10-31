//! Read-only inventory of active Flash data. The catalog is a hint, not proof
//! that a game has data; cleanup targets always come from existing files.
use super::{controls::Record, experience::hash};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File},
    io::Read,
    path::{Component, Path, PathBuf},
};

const RECORD_LIMIT: u64 = 64 * 1024;

#[derive(Clone, Debug, Default)]
pub struct Entry {
    pub key: String,
    pub title: String,
    pub save_ids: Vec<String>,
    pub control_ids: Vec<String>,
    pub movie_path: Option<PathBuf>,
    pub unavailable: bool,
    pub has_saves: bool,
    pub has_controls: bool,
}

/// Launcher-provided Flash roots, followed by current-card compatibility
/// locations. These are read-only hints; saves paths never imply a ROM root.
pub fn rom_roots() -> Vec<PathBuf> {
    let mut roots = Vec::new();
    if let Some(path) = std::env::var_os("RUFFLE_FLASH_DIR").filter(|p| !p.is_empty()) {
        roots.push(PathBuf::from(path));
    }
    if let Some(path) = std::env::var_os("ROMS_PATH").filter(|p| !p.is_empty()) {
        let path = PathBuf::from(path);
        roots.extend([path.join("flash"), path.join("FLASH")]);
    }
    if let Some(path) = std::env::var_os("SDCARD_PATH").filter(|p| !p.is_empty()) {
        let path = PathBuf::from(path).join("Roms");
        roots.extend([path.join("flash"), path.join("FLASH")]);
    }
    let mut seen = BTreeSet::new();
    roots.retain(|path| plain_absolute(path) && seen.insert(path.clone()));
    roots
}

#[derive(Default)]
struct Catalog {
    title: String,
    control_id: Option<String>,
    path: Option<PathBuf>,
}

#[derive(Default)]
struct Controls {
    identities: BTreeSet<String>,
}

#[derive(Default)]
struct Group {
    saves: BTreeSet<String>,
    controls: BTreeSet<String>,
    titles: BTreeSet<String>,
    // New catalog paths take precedence over paths recovered from old maps.
    catalog_paths: BTreeSet<PathBuf>,
    identities: BTreeSet<String>,
}

/// Optional third catalog line: `path=` followed by lowercase hexadecimal
/// UTF-8 bytes of the absolute, unescaped filesystem path (not a file URL).
pub fn encode_movie_path(path: &Path) -> Option<String> {
    let text = path.to_str()?;
    if !plain_absolute(path) || text.len() > 4096 || text.chars().any(char::is_control) {
        return None;
    }
    Some(format!("path={}", hex(text.as_bytes())))
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(text: &str) -> Option<String> {
    if text.is_empty() || text.len() > 8192 || text.len() % 2 != 0 {
        return None;
    }
    let bytes = text.as_bytes().chunks_exact(2).map(|pair| {
        let a = (pair[0] as char).to_digit(16)?;
        let b = (pair[1] as char).to_digit(16)?;
        Some((a * 16 + b) as u8)
    }).collect::<Option<Vec<_>>>()?;
    String::from_utf8(bytes).ok()
}

fn control_id(id: &str) -> bool {
    id.len() == 16 && id.bytes().all(|b| b.is_ascii_hexdigit())
}

fn save_id(id: &str) -> bool {
    let Some((digest, size)) = id.split_once('-') else { return false; };
    digest.len() == 32 && digest.bytes().all(|b| b.is_ascii_hexdigit())
        && !size.is_empty() && size.bytes().all(|b| b.is_ascii_digit())
        && size.parse::<u64>().is_ok()
}

fn no_symlinks(path: &Path) -> bool {
    path.ancestors().all(|ancestor| {
        fs::symlink_metadata(ancestor).map(|m| !m.file_type().is_symlink()).unwrap_or(true)
    })
}

fn regular(path: &Path) -> bool {
    no_symlinks(path) && fs::symlink_metadata(path).is_ok_and(|m| m.is_file())
}

fn directory(path: &Path) -> bool {
    no_symlinks(path) && fs::symlink_metadata(path).is_ok_and(|m| m.is_dir())
}

fn children(path: &Path) -> Vec<PathBuf> {
    if !directory(path) { return Vec::new(); }
    fs::read_dir(path).map(|items| items.flatten().map(|item| item.path()).collect()).unwrap_or_default()
}

fn read_small(path: &Path) -> Option<String> {
    if !regular(path) { return None; }
    let mut text = String::new();
    File::open(path).ok()?.take(RECORD_LIMIT + 1).read_to_string(&mut text).ok()?;
    (text.len() as u64 <= RECORD_LIMIT).then_some(text)
}

fn plain_absolute(path: &Path) -> bool {
    path.is_absolute() && path.components().all(|c| !matches!(c, Component::ParentDir | Component::CurDir))
}

fn relative_movie(path: &str) -> Option<&str> {
    let relative = path.strip_prefix("flash/")?;
    // Reject separators and dot segments which have platform-dependent meaning.
    if relative.is_empty() || relative.contains(['\\', ':'])
        || relative.chars().any(char::is_control)
        || relative.split('/').any(|part| part.is_empty() || part == "." || part == "..") {
        return None;
    }
    Some(relative)
}

fn permitted(path: &Path, roots: &[PathBuf]) -> bool {
    plain_absolute(path) && path.extension().and_then(|e| e.to_str()).is_some_and(|e| e.eq_ignore_ascii_case("swf"))
        && roots.iter().any(|root| plain_absolute(root) && path.strip_prefix(root).is_ok_and(|p| {
            !p.as_os_str().is_empty() && p.components().all(|c| matches!(c, Component::Normal(_)))
        }))
}

fn path_identity(path: &Path) -> Option<String> {
    let path = path.to_str()?.replace('\\', "/");
    let lower = path.to_ascii_lowercase();
    let logical = lower.find("/roms/flash/").map(|i| format!("flash/{}", &path[i + 12..])).unwrap_or(path);
    Some(hex(logical.as_bytes()))
}

fn matching_path(path: &Path, id: &str, roots: &[PathBuf]) -> bool {
    permitted(path, roots) && path_identity(path).is_some_and(|identity| format!("{:016x}", hash(identity.as_bytes())) == id)
}

fn identity_paths(identity: &str, id: &str, roots: &[PathBuf]) -> Vec<PathBuf> {
    if format!("{:016x}", hash(identity.as_bytes())) != id { return Vec::new(); }
    let Some(text) = unhex(identity) else { return Vec::new(); };
    if let Some(relative) = relative_movie(&text) {
        return roots.iter().map(|root| root.join(relative)).filter(|path| permitted(path, roots)).collect();
    }
    let path = PathBuf::from(text);
    if permitted(&path, roots) { vec![path] } else { Vec::new() }
}

fn map_identity(path: &Path, id: &str) -> Option<String> {
    let text = read_small(path)?;
    let identity = match Record::decode(&text) {
        Ok(record) => record.identity,
        Err(_) => {
            // A damaged mapping still needs to be manageable. Recover its
            // identity only when the field is unique and the directory hash
            // agrees; this never turns the damaged mapping into valid input.
            let mut values = text.lines().filter_map(|line| line.strip_prefix("identity="));
            let identity = values.next()?.to_string();
            if values.next().is_some() { return None; }
            identity
        }
    };
    unhex(&identity)?;
    (format!("{:016x}", hash(identity.as_bytes())) == id).then_some(identity)
}

fn control_files(directory: &Path) -> Vec<PathBuf> {
    let mut files = vec![directory.join("selected.rmap"), directory.join("selected.rmap.bak")];
    for slot in 1..=10 {
        files.push(directory.join(format!("slot-{slot:02}.rmap")));
        files.push(directory.join(format!("slot-{slot:02}.rmap.bak")));
    }
    files.into_iter().filter(|path| regular(path)).collect()
}

fn scan_controls(root: &Path) -> BTreeMap<String, Controls> {
    let mut result: BTreeMap<String, Controls> = BTreeMap::new();
    for path in children(&root.join("slots")) {
        let Some(id) = path.file_name().and_then(|s| s.to_str()).filter(|id| control_id(id)) else { continue; };
        if !directory(&path) { continue; }
        for file in control_files(&path) {
            let entry = result.entry(id.into()).or_default();
            if let Some(identity) = map_identity(&file, id) { entry.identities.insert(identity); }
        }
    }
    for path in children(&root.join("games")) {
        let Some(name) = path.file_name().and_then(|s| s.to_str()) else { continue; };
        let Some(id) = name.strip_suffix(".rmap.bak").or_else(|| name.strip_suffix(".rmap")).filter(|id| control_id(id)) else { continue; };
        if !regular(&path) { continue; }
        let entry = result.entry(id.into()).or_default();
        if let Some(identity) = map_identity(&path, id) { entry.identities.insert(identity); }
    }
    result
}

fn scan_saves(root: &Path) -> BTreeSet<String> {
    children(&root.join("games")).into_iter().filter_map(|path| {
        let id = path.file_name()?.to_str()?;
        if !save_id(id) || !directory(&path) { return None; }
        (1..=5).any(|slot| regular(&path.join(format!("slot-{slot}.rls")))
            || regular(&path.join(format!("slot-{slot}.rls.bak")))).then(|| id.to_string())
    }).collect()
}

fn catalog(root: &Path, roots: &[PathBuf]) -> BTreeMap<String, Catalog> {
    let mut result = BTreeMap::new();
    for path in children(&root.join("catalog")) {
        let Some(id) = path.file_name().and_then(|s| s.to_str()).and_then(|s| s.strip_suffix(".meta")).filter(|id| save_id(id)) else { continue; };
        let Some(text) = read_small(&path) else { continue; };
        let mut lines = text.lines();
        let title = lines.next().unwrap_or("").chars().filter(|c| !c.is_control()).take(256).collect();
        let control_id = lines.next().filter(|id| control_id(id)).map(str::to_string);
        let movie = lines.next().and_then(|line| line.strip_prefix("path=")).and_then(unhex).map(PathBuf::from)
            .filter(|path| control_id.as_deref().is_some_and(|id| matching_path(path, id, roots)));
        result.insert(id.to_string(), Catalog { title, control_id, path: movie });
    }
    result
}

/// `progress_root` is `slots-v1`; `rom_roots` contains actual allowed Flash
/// directories, such as `$ROMS_PATH/flash` and its legacy `FLASH` counterpart.
/// No SWF content is read. A shared content save belongs to the last cataloged
/// path only, so no save or control target appears in two cleanup entries.
pub fn entries(progress_root: &Path, controls_root: &Path, rom_roots: &[PathBuf]) -> Vec<Entry> {
    let saves = scan_saves(progress_root);
    let controls = scan_controls(controls_root);
    let catalogs = catalog(progress_root, rom_roots);
    let mut groups: BTreeMap<String, Group> = BTreeMap::new();
    let mut assigned_saves = BTreeSet::new();
    for (id, control) in controls {
        let group = groups.entry(format!("game:{id}")).or_default();
        group.controls.insert(id);
        group.identities.extend(control.identities);
    }
    // A catalog record can name a control-only game; this preserves its title
    // even when the corresponding save was never created or was cleared.
    for (id, entry) in catalogs {
        let key = entry.control_id.as_ref().map(|id| format!("game:{id}")).unwrap_or_else(|| format!("save:{id}"));
        if !saves.contains(&id) && !groups.contains_key(&key) { continue; }
        let group = groups.entry(key).or_default();
        if saves.contains(&id) { assigned_saves.insert(id.clone()); group.saves.insert(id); }
        if !entry.title.is_empty() { group.titles.insert(entry.title); }
        if let Some(path) = entry.path {
            if let Some(identity) = path_identity(&path) { group.identities.insert(identity); }
            group.catalog_paths.insert(path);
        }
    }
    // Files survive catalog loss, and must remain visible for cleanup.
    for id in saves {
        if !assigned_saves.contains(&id) {
            groups.entry(format!("save:{id}")).or_default().saves.insert(id);
        }
    }
    let mut result: Vec<_> = groups.into_iter().map(|(key, group)| {
        let id = key.split_once(':').unwrap().1;
        let mut paths: Vec<_> = group.catalog_paths.into_iter().collect();
        let mut known: BTreeSet<_> = paths.iter().cloned().collect();
        for path in group.identities.iter().flat_map(|identity| identity_paths(identity, id, rom_roots)) {
            if known.insert(path.clone()) { paths.push(path); }
        }
        let existing = paths.iter().find(|path| regular(path));
        let unavailable = !paths.is_empty() && existing.is_none();
        let movie_path = existing.cloned().or_else(|| paths.into_iter().next());
        let title = group.titles.into_iter().next().or_else(|| movie_path.as_ref().and_then(|p| p.file_stem()).map(|s| s.to_string_lossy().into_owned()))
            .or_else(|| group.identities.iter().filter_map(|identity| unhex(identity)).find_map(|text| {
                relative_movie(&text).and_then(|relative| Path::new(relative).file_stem()).and_then(|s| s.to_str()).map(str::to_string)
            }))
            .unwrap_or_else(|| format!("Flash · {}", &id[..id.len().min(12)]));
        Entry { key, title, has_saves: !group.saves.is_empty(), has_controls: !group.controls.is_empty(),
            save_ids: group.saves.into_iter().collect(), control_ids: group.controls.into_iter().collect(), movie_path, unavailable }
    }).collect();
    result.sort_by(|a, b| a.title.cmp(&b.title).then(a.key.cmp(&b.key)));
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::{controls::{Mapping, Store}, experience::game_id};

    struct Fixture { root: PathBuf, progress: PathBuf, controls: PathBuf, roms: PathBuf }
    impl Fixture {
        fn new() -> Self {
            static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let root = std::env::temp_dir().join(format!("ruffle-manager-{}-{}-{}", std::process::id(),
                std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos(),
                NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)));
            let progress = root.join("slots-v1");
            let controls = root.join("config/controls");
            let roms = root.join("Roms/flash");
            fs::create_dir_all(&roms).unwrap();
            Self { root, progress, controls, roms }
        }
        fn list(&self) -> Vec<Entry> { entries(&self.progress, &self.controls, &[self.roms.clone()]) }
        fn store(&self, name: &str) -> Store { Store::new(self.controls.clone(), &self.roms.join(name)) }
        fn control_id(&self, name: &str) -> String { self.store(name).game.file_stem().unwrap().to_string_lossy().into_owned() }
        fn write(&self, path: &Path, bytes: &[u8]) { fs::create_dir_all(path.parent().unwrap()).unwrap(); fs::write(path, bytes).unwrap(); }
        fn save(&self, content: &[u8], suffix: &str) -> String {
            let id = game_id(content);
            self.write(&self.progress.join("games").join(&id).join(suffix), b"damaged or valid native data");
            id
        }
        fn meta(&self, id: &str, name: &str, with_path: bool) {
            let third = if with_path { format!("{}\n", encode_movie_path(&self.roms.join(name)).unwrap()) } else { String::new() };
            self.write(&self.progress.join("catalog").join(format!("{id}.meta")), format!("{name}\n{}\n{third}", self.control_id(name)).as_bytes());
        }
        fn mapping(&self, name: &str, selected: bool, backup: bool) {
            let store = self.store(name);
            let record = Record { name: "Slot 1".into(), identity: store.identity.clone(), mapping: Mapping::group(2) };
            let path = if selected { store.game_slot(0).parent().unwrap().join("selected.rmap") } else { store.game_slot(0) };
            self.write(&if backup { path.with_extension("rmap.bak") } else { path }, record.encode().as_bytes());
        }
    }
    impl Drop for Fixture { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.root); } }

    #[test]
    fn catalog_and_archives_alone_do_not_create_entries() {
        let f = Fixture::new();
        f.meta(&game_id(b"launched only"), "Launched.swf", true);
        f.write(&f.progress.parent().unwrap().join("cleared-data/old/slot-1.rls"), b"old");
        assert!(f.list().is_empty());
    }

    #[test]
    fn selected_only_empty_mappings_and_backups_are_manageable() {
        let f = Fixture::new();
        f.mapping("Selected.swf", true, false);
        f.mapping("Backup.swf", false, true);
        let list = f.list();
        assert_eq!(list.len(), 2);
        assert!(list.iter().all(|e| e.has_controls && !e.has_saves && e.unavailable));
        assert!(list.iter().all(|e| e.control_ids.len() == 1 && e.save_ids.is_empty()));
    }

    #[test]
    fn standalone_saves_survive_catalog_loss_without_false_deleted_status() {
        let f = Fixture::new();
        let id = f.save(b"orphan", "slot-3.rls.bak");
        let list = f.list();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].save_ids, [id]);
        assert!(list[0].has_saves && !list[0].has_controls);
        assert!(!list[0].unavailable);
        assert!(list[0].movie_path.is_none());
    }

    #[test]
    fn new_catalog_paths_locate_deleted_games_and_existing_games() {
        let f = Fixture::new();
        let id = f.save(b"saved", "slot-1.rls");
        f.meta(&id, "游戏 空格.swf", true);
        assert!(f.list()[0].unavailable);
        f.write(&f.roms.join("游戏 空格.swf"), b"swf");
        let list = f.list();
        assert!(!list[0].unavailable);
        assert_eq!(list[0].movie_path.as_ref().unwrap(), &f.roms.join("游戏 空格.swf"));
    }

    #[test]
    fn old_catalog_recovers_relative_path_and_refresh_hides_cleared_data() {
        let f = Fixture::new();
        let id = f.save(b"old", "slot-1.rls");
        f.meta(&id, "Nested/Old.swf", false);
        f.mapping("Nested/Old.swf", false, false);
        f.write(&f.roms.join("Nested/Old.swf"), b"swf");
        let list = f.list();
        assert_eq!(list.len(), 1);
        assert!(!list[0].unavailable);
        assert!(list[0].has_saves && list[0].has_controls);
        fs::remove_dir_all(f.progress.join("games")).unwrap();
        fs::remove_dir_all(f.controls.join("slots")).unwrap();
        assert!(f.list().is_empty());
    }

    #[test]
    fn legacy_control_backup_and_damaged_mapping_keep_recoverable_names() {
        let f = Fixture::new();
        let store = f.store("Legacy.swf");
        let record = Record { name: "Legacy".into(), identity: store.identity.clone(), mapping: Mapping::group(0) };
        f.write(&store.game.with_extension("rmap.bak"), record.encode().as_bytes());
        let store = f.store("Damaged.swf");
        f.write(&store.game_slot(0), format!("identity={}\nbroken\n", store.identity).as_bytes());
        let list = f.list();
        assert_eq!(list.len(), 2);
        assert!(list.iter().all(|e| e.has_controls && e.unavailable));
        assert!(list.iter().any(|e| e.title == "Legacy"));
        assert!(list.iter().any(|e| e.title == "Damaged"));
    }

    #[test]
    fn new_catalog_can_find_the_same_relative_path_in_a_legacy_flash_root() {
        let f = Fixture::new();
        let id = f.save(b"legacy directory", "slot-1.rls");
        f.meta(&id, "Moved.swf", true);
        let legacy = f.root.join("Roms/FLASH");
        f.write(&legacy.join("Moved.swf"), b"swf");
        let list = entries(&f.progress, &f.controls, &[f.roms.clone(), legacy.clone()]);
        assert_eq!(list.len(), 1);
        assert!(!list[0].unavailable);
        // On case-insensitive filesystems both spellings address the same file.
        assert!(list[0].movie_path.as_ref().is_some_and(|p| regular(p)));
    }

    #[test]
    fn unknown_old_paths_and_corrupt_files_remain_manageable() {
        let f = Fixture::new();
        let id = f.save(b"unknown old path", "slot-5.rls");
        f.meta(&id, "Old.swf", false);
        let store = f.store("Old.swf");
        f.write(&store.game_slot(0), b"corrupt");
        let list = f.list();
        assert_eq!(list.len(), 1);
        assert!(list[0].has_saves && list[0].has_controls);
        assert!(!list[0].unavailable);
        assert!(list[0].movie_path.is_none());
    }

    #[test]
    fn versions_at_one_path_group_and_shared_content_targets_are_not_duplicated() {
        let f = Fixture::new();
        for bytes in [b"version one".as_slice(), b"version two".as_slice()] {
            let id = f.save(bytes, "slot-1.rls");
            f.meta(&id, "Last.swf", true);
        }
        f.mapping("Last.swf", false, false);
        f.mapping("Other.swf", false, false);
        let list = f.list();
        assert_eq!(list.len(), 2);
        assert_eq!(list.iter().filter(|e| e.has_saves).count(), 1);
        assert_eq!(list.iter().map(|e| e.save_ids.len()).sum::<usize>(), 2);
        assert_eq!(list.iter().map(|e| e.control_ids.len()).sum::<usize>(), 2);
    }

    #[test]
    fn mismatched_hash_and_traversal_cannot_claim_a_movie_path() {
        let f = Fixture::new();
        let store = f.store("Safe.swf");
        let record = Record { name: "Slot 1".into(), identity: hex(b"flash/../outside.swf"), mapping: Mapping::group(0) };
        f.write(&store.game_slot(0), record.encode().as_bytes());
        let id = f.save(b"unsafe meta", "slot-1.rls");
        let outside = f.root.join("outside.swf");
        f.write(&outside, b"swf");
        f.write(&f.progress.join("catalog").join(format!("{id}.meta")), format!("Safe\n{}\n{}\n", f.control_id("Safe.swf"), encode_movie_path(&outside).unwrap()).as_bytes());
        let list = f.list();
        assert_eq!(list.len(), 1);
        assert!(list[0].movie_path.is_none());
        assert!(!list[0].unavailable);
        // An identity whose hash does agree still cannot use dot traversal.
        let forged_id = format!("{:016x}", hash(record.identity.as_bytes()));
        assert!(identity_paths(&record.identity, &forged_id, &[f.roms.clone()]).is_empty());
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_data_and_movies_are_never_followed() {
        use std::os::unix::fs::symlink;
        let f = Fixture::new();
        let id = f.save(b"linked", "slot-1.rls");
        f.meta(&id, "Linked.swf", true);
        let outside = f.root.join("outside.swf");
        f.write(&outside, b"swf");
        symlink(&outside, f.roms.join("Linked.swf")).unwrap();
        assert!(f.list()[0].unavailable);
        fs::remove_file(f.progress.join("games").join(&id).join("slot-1.rls")).unwrap();
        symlink(&outside, f.progress.join("games").join(&id).join("slot-1.rls")).unwrap();
        assert!(f.list().is_empty());
    }
}
