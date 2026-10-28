//! Global UI language, initialized once from Leaf and then owned by the user.
use std::{cell::Cell, fs, path::{Path, PathBuf}};

#[path = "translations.rs"]
mod translations;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Language { #[default] Chinese, English }

impl Language {
    pub const ALL: [Self; 2] = [Self::Chinese, Self::English];
    pub fn code(self) -> &'static str { match self { Self::Chinese => "zh-CN", Self::English => "en" } }
    pub fn label(self) -> &'static str { match self { Self::Chinese => "简体中文", Self::English => "English" } }

    fn parse(code: &str) -> Option<Self> {
        match code { "zh-CN" => Some(Self::Chinese), "en" => Some(Self::English), _ => None }
    }

    pub fn detect<'a>(codes: impl IntoIterator<Item = &'a str>) -> Self {
        // The first nonempty source is authoritative, even if unsupported.
        let code = codes.into_iter().map(str::trim).find(|s| !s.is_empty()).unwrap_or("en");
        let base = code.split(['-', '_', '.', '@']).next().unwrap_or("");
        if base.eq_ignore_ascii_case("zh") { Self::Chinese } else { Self::English }
    }
}

// The frontend and Flash UI run on one thread. Thread-local state also keeps
// concurrent host tests from changing each other's language.
thread_local! { static CURRENT: Cell<Language> = const { Cell::new(Language::Chinese) }; }
pub fn current() -> Language { CURRENT.with(Cell::get) }
pub fn set(language: Language) { CURRENT.with(|value| value.set(language)); }
pub fn is_english() -> bool { current() == Language::English }
pub fn t(text: &str) -> &str {
    if is_english() { translations::english(text).unwrap_or(text) } else { text }
}

/// Both format literals are supplied at the call site, retaining Rust's format
/// checking and named captures. User-provided titles/paths are never translated.
#[macro_export]
macro_rules! tr {
    ($zh:literal, $en:literal $(, $args:expr)* $(,)?) => {
        if $crate::session::i18n::is_english() { format!($en $(, $args)*) }
        else { format!($zh $(, $args)*) }
    };
}

pub fn path(controls_root: &Path) -> PathBuf {
    controls_root.parent().unwrap_or(controls_root).join("language.conf")
}

pub fn save(path: &Path, language: Language) -> Result<(), String> {
    super::experience::atomic(path, format!("version=1\nlanguage={}\n", language.code()).as_bytes())
}

pub fn select(path: &Path, language: Language) -> Result<(), String> {
    save(path, language)?;
    set(language);
    Ok(())
}

fn decode(bytes: &[u8]) -> Option<Language> {
    if bytes.len() > 128 { return None; }
    let text = std::str::from_utf8(bytes).ok()?;
    let mut version = None;
    let mut language = None;
    for line in text.lines() {
        let (key, value) = line.split_once('=')?;
        match key {
            "version" if version.is_none() => version = Some(value),
            "language" if language.is_none() => language = Language::parse(value),
            _ => return None,
        }
        if key == "language" && language.is_none() { return None; }
    }
    if version == Some("1") { language } else { None }
}

pub fn load_or_detect(path: &Path, detected: Language) -> Result<Language, String> {
    match fs::read(path) {
        Ok(bytes) => if let Some(language) = decode(&bytes) { return Ok(language); },
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {},
        Err(error) => return Err(error.to_string()),
    }
    save(path, detected)?;
    Ok(detected)
}

pub fn initialize(path: &Path) -> Result<(), String> {
    let codes: Vec<_> = ["UMRK_LANGUAGE", "JAWAKA_LANGUAGE", "RUFFLE_SYSTEM_LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"]
        .iter().filter_map(|key| std::env::var(key).ok()).collect();
    let detected = Language::detect(codes.iter().map(String::as_str));
    set(detected);
    set(load_or_detect(path, detected)?);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn leaf_language_priority_and_locale_variants() {
        for code in ["zh", "zh_CN", "ZH-cn.UTF-8", "zh-Hant", "zh_TW@variant"] {
            assert_eq!(Language::detect([code, "en"]), Language::Chinese);
        }
        for code in ["en", "en_US.UTF-8", "fr", "ja", "C.UTF-8", "zho"] {
            assert_eq!(Language::detect([code, "zh-CN"]), Language::English);
        }
        assert_eq!(Language::detect(["", "  ", "zh_CN"]), Language::Chinese);
        assert_eq!(Language::detect([]), Language::English);
    }

    #[test]
    fn first_launch_is_saved_and_later_system_changes_are_ignored() {
        let root = std::env::temp_dir().join(format!("ruffle-language-{}-{}", std::process::id(), super::super::experience::now()));
        let file = path(&root.join("controls"));
        assert_eq!(load_or_detect(&file, Language::English).unwrap(), Language::English);
        assert_eq!(load_or_detect(&file, Language::Chinese).unwrap(), Language::English);
        save(&file, Language::Chinese).unwrap();
        assert_eq!(load_or_detect(&file, Language::English).unwrap(), Language::Chinese);
        fs::write(&file, b"broken").unwrap();
        assert_eq!(load_or_detect(&file, Language::English).unwrap(), Language::English);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn invalid_or_duplicate_language_fields_are_rejected() {
        for bytes in [b"version=2\nlanguage=en\n".as_slice(), b"language=fr\nversion=1\n",
            b"version=1\nlanguage=en\nlanguage=zh-CN\n", b"version=1\n",
            b"version=1\nlanguage=en\nextra=x\n", b"version=1\nlanguage=fr\nlanguage=en\n"] {
            assert!(decode(bytes).is_none());
        }
        assert_eq!(decode(b"language=en\r\nversion=1\r\n"), Some(Language::English));
        assert!(decode(&[b'x'; 129]).is_none());
    }

    #[test]
    fn translated_labels_formats_and_user_text() {
        set(Language::English);
        assert_eq!(t("画面设置"), "Display settings");
        let title = "中文游戏名.swf";
        assert_eq!(t(title), title);
        let error = "disk full";
        assert_eq!(crate::tr!("保存失败：{error}", "Save failed: {error}"), "Save failed: disk full");
        set(Language::Chinese);
        assert_eq!(t("画面设置"), "画面设置");
    }

    #[test]
    fn failed_save_keeps_the_previous_language() {
        set(Language::Chinese);
        let file = std::env::temp_dir().join(format!("ruffle-language-blocked-{}", std::process::id()));
        fs::write(&file, b"block directory creation").unwrap();
        assert!(select(&file.join("language.conf"), Language::English).is_err());
        assert_eq!(current(), Language::Chinese);
        assert_eq!(fs::read(&file).unwrap(), b"block directory creation");
        fs::remove_file(file).unwrap();
    }

    #[test]
    fn english_covers_menu_controls_help_and_save_states() {
        use super::super::{controls, cursor, experience};
        set(Language::English);
        let check = |text: &str| assert!(!text.chars().any(|c| ('\u{4e00}'..='\u{9fff}').contains(&c)), "Untranslated: {text}");
        let mut ui = experience::Ui::default();
        for page in [experience::Page::Saves, experience::Page::Settings, experience::Page::Renderer,
            experience::Page::Manager, experience::Page::Confirm, experience::Page::About, experience::Page::Shortcuts] {
            ui.open(page);
            check(&ui.heading());
            for row in ui.rows() { if row != "语言 / Language" { check(&row); } }
        }
        for page in [controls::Page::Edit, controls::Page::Picker, controls::Page::Cursor, controls::Page::Confirm] {
            let mut controls = controls::Controls::default();
            controls.page = page;
            check(controls.title()); check(controls.hint()); check(&controls.detail());
            for row in controls.lines() { check(&row); }
        }
        for label in controls::CATEGORIES.iter().chain(experience::SHORTCUT_TABS.iter()).chain(experience::SHORTCUT_NOTES.iter()) { check(t(label)); }
        for (a,b,c) in experience::SHORTCUTS.iter().flatten().chain(experience::BACKGROUND.iter()) { check(t(a)); check(t(b)); check(t(c)); }
        for slot in [experience::Slot::default(), experience::Slot{broken:true,..Default::default()},
            experience::Slot{time:1,recovered:true,..Default::default()}] { check(&slot.detail()); }
        check(cursor::Style::RingDot.name()); check(cursor::Color::White.name());
        set(Language::Chinese);
    }

    #[test]
    fn language_page_returns_to_its_entry_and_selection_waits_for_save() {
        use super::super::{controls::Nav, experience::{Command, Page, Ui}};
        for app in [false, true] {
            let mut ui = Ui{app,started:!app,language:Language::English,..Default::default()};
            if app { ui.open(Page::Manager); ui.selected=5; ui.nav(Nav::A); }
            else { ui.open(Page::Language); }
            assert_eq!(ui.page, Page::Language);
            assert_eq!(ui.selected, 1);
            ui.nav(Nav::Up);
            assert!(matches!(ui.nav(Nav::A), Command::Language(Language::Chinese)));
            assert_eq!(ui.language, Language::English);
            assert!(matches!(ui.nav(Nav::B), Command::None));
            assert_eq!(ui.page, if app {Page::Manager} else {Page::Home});
            ui.open(Page::Language); ui.selected=2; ui.nav(Nav::A);
            assert_eq!(ui.page, if app {Page::Manager} else {Page::Home});
        }
    }
}
