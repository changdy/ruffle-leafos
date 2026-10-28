use std::cell::OnceCell;
use std::fs;
use std::path::{Path, PathBuf};

use ruffle_core::backend::ui::{
    DialogResultFuture, FileFilter, FontDefinition, FullscreenError, LanguageIdentifier,
    MouseCursor, NullUiBackend, UiBackend,
};
use ruffle_core::font::{FontFileData, FontQuery};
use url::Url;

#[derive(Clone, Debug)]
struct SystemFontFace {
    path: PathBuf,
    index: u32,
    names: Vec<String>,
    is_bold: bool,
    is_italic: bool,
    is_cjk: bool,
}

pub struct SdlUiBackend {
    fallback: NullUiBackend,
    fonts: OnceCell<Vec<SystemFontFace>>,
    keyboard: crate::session::virtual_keyboard::Request,
}

impl SdlUiBackend {
    pub fn new() -> Self {
        Self {
            fallback: NullUiBackend::new(),
            fonts: OnceCell::new(),
            keyboard: Default::default(),
        }
    }

    pub fn keyboard_request(&self) -> crate::session::virtual_keyboard::Request {
        self.keyboard.clone()
    }

    fn fonts(&self) -> &[SystemFontFace] {
        self.fonts.get_or_init(scan_system_fonts)
    }

    fn register_font(font: &SystemFontFace, register: &mut dyn FnMut(FontDefinition)) {
        let name = font
            .names
            .first()
            .cloned()
            .unwrap_or_else(|| "System Font".to_string());
        Self::register_font_as(font, name, font.is_bold, font.is_italic, register);
    }

    fn register_font_as(
        font: &SystemFontFace,
        name: String,
        is_bold: bool,
        is_italic: bool,
        register: &mut dyn FnMut(FontDefinition),
    ) {
        match fs::read(&font.path) {
            Ok(bytes) => {
                register(FontDefinition::FontFile {
                    name,
                    is_bold,
                    is_italic,
                    data: FontFileData::new(bytes),
                    index: font.index,
                });
            }
            Err(error) => {
                log::warn!("Failed to read system font {:?}: {error}", font.path);
            }
        }
    }

    fn name_matches(query: &FontQuery, font: &SystemFontFace) -> bool {
        let query_name = normalize_font_name(&query.name);
        if query_name.is_empty() {
            return false;
        }

        font.names
            .iter()
            .map(|name| normalize_font_name(name))
            .any(|name| {
                name == query_name || name.contains(&query_name) || query_name.contains(&name)
            })
            || font
                .path
                .file_stem()
                .and_then(|name| name.to_str())
                .map(normalize_font_name)
                .is_some_and(|name| name.contains(&query_name) || query_name.contains(&name))
    }

    fn style_score(query: &FontQuery, font: &SystemFontFace) -> u8 {
        u8::from(query.is_bold != font.is_bold) + u8::from(query.is_italic != font.is_italic)
    }

    fn misans_font(&self) -> Option<&SystemFontFace> {
        self.fonts().iter().find(|font| {
            font.names
                .iter()
                .map(|name| normalize_font_name(name))
                .any(|name| name == "misans" || name == "misansnormal")
                || font
                    .path
                    .file_name()
                    .and_then(|name| name.to_str())
                    .map(|name| name.eq_ignore_ascii_case("MiSans-Normal.ttf"))
                    .unwrap_or(false)
        })
    }

    fn dejavu_fallback_font(&self) -> Option<&SystemFontFace> {
        self.fonts().iter().find(|font| {
            font.path
                .file_name()
                .and_then(|name| name.to_str())
                .map(|name| name.eq_ignore_ascii_case("DejaVuSansMono.ttf"))
                .unwrap_or(false)
        })
    }

    fn cjk_fallback_font(&self) -> Option<&SystemFontFace> {
        self.misans_font()
            .or_else(|| self.dejavu_fallback_font())
    }

    pub fn cjk_font_names(&self) -> Vec<String> {
        let mut names = Vec::new();
        let mut push_font_names = |names: &mut Vec<String>, font: &SystemFontFace| {
            for name in &font.names {
                if !names.iter().any(|existing| existing == name) {
                    names.push(name.clone());
                }
            }
        };
        if let Some(font) = self.misans_font() {
            push_font_names(&mut names, font);
        } else if let Some(font) = self.dejavu_fallback_font() {
            push_font_names(&mut names, font);
        }
        for font in self.fonts().iter().filter(|font| font.is_cjk) {
            if let Some(name) = font.names.first() {
                if !names.iter().any(|existing| existing == name) {
                    names.push(name.clone());
                }
            }
        }
        names
    }
}

impl UiBackend for SdlUiBackend {
    fn mouse_visible(&self) -> bool {
        self.fallback.mouse_visible()
    }

    fn set_mouse_visible(&mut self, visible: bool) {
        self.fallback.set_mouse_visible(visible);
    }

    fn set_mouse_cursor(&mut self, cursor: MouseCursor) {
        self.fallback.set_mouse_cursor(cursor);
    }

    fn clipboard_content(&mut self) -> String {
        self.fallback.clipboard_content()
    }

    fn set_clipboard_content(&mut self, content: String) {
        self.fallback.set_clipboard_content(content);
    }

    fn set_fullscreen(&mut self, is_full: bool) -> Result<(), FullscreenError> {
        self.fallback.set_fullscreen(is_full)
    }

    fn display_root_movie_download_failed_message(&self, invalid_swf: bool, fetched_error: String) {
        self.fallback
            .display_root_movie_download_failed_message(invalid_swf, fetched_error);
    }

    fn message(&self, message: &str) {
        self.fallback.message(message);
    }

    fn open_virtual_keyboard(&self) {
        self.keyboard.open();
    }

    fn close_virtual_keyboard(&self) {
        self.keyboard.close();
    }

    fn language(&self) -> LanguageIdentifier {
        self.fallback.language()
    }

    fn display_unsupported_video(&self, url: Url) {
        self.fallback.display_unsupported_video(url);
    }

    fn load_device_font(&self, query: &FontQuery, register: &mut dyn FnMut(FontDefinition)) {
        let mut candidates: Vec<_> = self
            .fonts()
            .iter()
            .filter(|font| Self::name_matches(query, font))
            .collect();
        candidates.sort_by_key(|font| Self::style_score(query, font));

        for font in candidates.into_iter().take(4) {
            Self::register_font(font, register);
        }

        if !self
            .fonts()
            .iter()
            .any(|font| Self::name_matches(query, font))
        {
            if let Some(font) = self.cjk_fallback_font() {
                Self::register_font_as(
                    font,
                    query.name.clone(),
                    query.is_bold,
                    query.is_italic,
                    register,
                );
            } else {
                log::warn!(
                    "System font {:?} not found and no CJK fallback font is available (MiSans-Normal.ttf / DejaVuSansMono.ttf)",
                    query.name
                );
            }
        }
    }

    fn sort_device_fonts(
        &self,
        query: &FontQuery,
        register: &mut dyn FnMut(FontDefinition),
    ) -> Vec<FontQuery> {
        let mut candidates: Vec<_> = self
            .fonts()
            .iter()
            .filter(|font| Self::name_matches(query, font))
            .collect();

        if candidates.is_empty() {
            if let Some(font) = self.misans_font() {
                candidates.push(font);
            } else if let Some(font) = self.dejavu_fallback_font() {
                candidates.push(font);
            } else {
                candidates = self.fonts().iter().filter(|font| font.is_cjk).collect();
            }
        }

        candidates.sort_by_key(|font| {
            (
                Self::style_score(query, font),
                !font.is_cjk,
                font.names.first().cloned().unwrap_or_default(),
            )
        });

        let mut queries = Vec::new();
        for font in candidates.into_iter().take(8) {
            Self::register_font(font, register);
            if let Some(name) = font.names.first() {
                queries.push(FontQuery::new(
                    query.font_type,
                    name.clone(),
                    font.is_bold,
                    font.is_italic,
                ));
            }
        }
        queries
    }

    fn display_file_open_dialog(&mut self, filters: Vec<FileFilter>) -> Option<DialogResultFuture> {
        self.fallback.display_file_open_dialog(filters)
    }

    fn display_file_save_dialog(
        &mut self,
        file_name: String,
        title: String,
    ) -> Option<DialogResultFuture> {
        self.fallback.display_file_save_dialog(file_name, title)
    }

    fn close_file_dialog(&mut self) {
        self.fallback.close_file_dialog();
    }
}

fn scan_system_fonts() -> Vec<SystemFontFace> {
    let mut fonts = Vec::new();
    for path in explicit_font_files() {
        if path.is_file() {
            read_font_faces(&path, &mut fonts);
        }
    }
    for root in font_roots() {
        visit_font_dir(&root, &mut fonts);
    }
    log::info!("Loaded metadata for {} system font faces", fonts.len());
    fonts
}

fn explicit_font_files() -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        paths.push(cwd.join("MiSans-Normal.ttf"));
        paths.push(cwd.join("assets/MiSans-Normal.ttf"));
        paths.push(cwd.join("DejaVuSansMono.ttf"));
        paths.push(cwd.join("assets/DejaVuSansMono.ttf"));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            paths.push(dir.join("MiSans-Normal.ttf"));
            paths.push(dir.join("DejaVuSansMono.ttf"));
        }
    }
    paths.push(PathBuf::from("/usr/share/fonts/TTF/DejaVuSansMono.ttf"));
    paths
}

fn font_roots() -> Vec<PathBuf> {
    let mut roots = vec![
        PathBuf::from("/usr/share/fonts"),
        PathBuf::from("/usr/local/share/fonts"),
    ];
    if let Some(home) = std::env::var_os("HOME") {
        roots.push(PathBuf::from(home).join(".local/share/fonts"));
    }
    roots
}

fn visit_font_dir(path: &Path, fonts: &mut Vec<SystemFontFace>) {
    let Ok(entries) = fs::read_dir(path) else {
        return;
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            visit_font_dir(&path, fonts);
        } else if is_font_file(&path) {
            read_font_faces(&path, fonts);
        }
    }
}

fn is_font_file(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .map(|extension| {
            matches!(
                extension.to_ascii_lowercase().as_str(),
                "ttf" | "otf" | "ttc" | "otc"
            )
        })
        .unwrap_or(false)
}

fn read_font_faces(path: &Path, fonts: &mut Vec<SystemFontFace>) {
    let Ok(data) = fs::read(path) else {
        return;
    };
    let face_count = ttf_parser::fonts_in_collection(&data).unwrap_or(1);
    for index in 0..face_count {
        let Ok(face) = ttf_parser::Face::parse(&data, index) else {
            continue;
        };

        let mut names = font_names(&face);
        if names.is_empty() {
            if let Some(stem) = path.file_stem().and_then(|stem| stem.to_str()) {
                names.push(stem.to_string());
            }
        }
        if names.is_empty() {
            continue;
        }

        let lower_names = names
            .iter()
            .map(|name| name.to_lowercase())
            .collect::<Vec<_>>();
        let path_lower = path.to_string_lossy().to_lowercase();
        let is_bold =
            lower_names.iter().any(|name| name.contains("bold")) || path_lower.contains("bold");
        let is_italic = lower_names
            .iter()
            .any(|name| name.contains("italic") || name.contains("oblique"))
            || path_lower.contains("italic")
            || path_lower.contains("oblique");
        let is_cjk = looks_like_cjk_font(&path_lower, &lower_names);

        fonts.push(SystemFontFace {
            path: path.to_path_buf(),
            index,
            names,
            is_bold,
            is_italic,
            is_cjk,
        });
    }
}

fn font_names(face: &ttf_parser::Face<'_>) -> Vec<String> {
    let mut names = Vec::new();
    for name in face.names() {
        if !matches!(
            name.name_id,
            ttf_parser::name_id::FAMILY
                | ttf_parser::name_id::FULL_NAME
                | ttf_parser::name_id::POST_SCRIPT_NAME
                | ttf_parser::name_id::TYPOGRAPHIC_FAMILY
        ) {
            continue;
        }
        if let Some(value) = name.to_string() {
            if !value.is_empty() && !names.iter().any(|name| name == &value) {
                names.push(value);
            }
        }
    }
    names
}

fn normalize_font_name(name: &str) -> String {
    name.chars()
        .filter(|ch| ch.is_alphanumeric() || *ch as u32 > 0x7f)
        .flat_map(char::to_lowercase)
        .collect()
}

fn looks_like_cjk_font(path: &str, names: &[String]) -> bool {
    const MARKERS: &[&str] = &[
        "cjk",
        "chinese",
        "sc",
        "tc",
        "cn",
        "gb",
        "source han",
        "sourcehan",
        "noto sans cjk",
        "noto serif cjk",
        "wenquanyi",
        "wqy",
        "droid sans fallback",
        "simsun",
        "simhei",
        "yahei",
        "mingliu",
        "songti",
        "heiti",
        "kaiti",
        "fangsong",
        "hiragino",
        "meiryo",
        "gothic",
        "mincho",
        "宋",
        "黑",
        "楷",
        "明",
        "微软雅黑",
    ];
    MARKERS
        .iter()
        .any(|marker| path.contains(marker) || names.iter().any(|name| name.contains(marker)))
}
