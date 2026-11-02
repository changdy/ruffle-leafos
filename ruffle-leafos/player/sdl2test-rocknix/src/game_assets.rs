//! Keep directory-game resources outside Leaf's recursive ROM scan.
use std::{fs::File, io, path::{Component, Path}};

pub fn open(game_dir: &Path, path: &Path) -> io::Result<File> {
    // Keep the movie URL (and therefore progress/control identities) unchanged.
    // Only map the selected game's assets subtree, never arbitrary ancestors.
    if let Ok(relative) = path.strip_prefix(game_dir) {
        let mut parts = relative.components();
        if parts.next() == Some(Component::Normal("assets".as_ref())) {
            let tail = parts.as_path();
            if !tail.as_os_str().is_empty()
                && tail.components().all(|part| matches!(part, Component::Normal(_)))
                && game_dir.join("_assets").is_dir()
            {
                // Prefer the new tree even if a merge upgrade left old assets.
                return File::open(game_dir.join("_assets").join(tail));
            }
        }
    }
    File::open(path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    #[test]
    fn private_assets_preserve_logical_paths_and_legacy_games_still_load() {
        let root = std::env::temp_dir().join(format!("ruffle-assets-{}", std::process::id()));
        std::fs::create_dir_all(root.join("_assets/nested")).unwrap();
        std::fs::create_dir_all(root.join("assets/nested")).unwrap();
        std::fs::write(root.join("_assets/nested/test.swf"), b"private").unwrap();
        std::fs::write(root.join("assets/nested/test.swf"), b"legacy").unwrap();
        let read = |path: &Path| {
            let mut bytes = Vec::new();
            open(&root, path).unwrap().read_to_end(&mut bytes).unwrap();
            bytes
        };
        assert_eq!(read(&root.join("assets/nested/test.swf")), b"private");
        assert!(open(&root, &root.join("assets/missing.swf")).is_err());
        // No prefix matching or parent traversal into the private tree.
        assert!(open(&root, &root.join("assets/../nested/test.swf")).is_err());
        assert!(open(&root, &root.join("other/assets/nested/test.swf")).is_err());
        std::fs::remove_dir_all(root.join("_assets")).unwrap();
        assert_eq!(read(&root.join("assets/nested/test.swf")), b"legacy");
        std::fs::remove_dir_all(root).unwrap();
    }
}
