//! Renderer preference and restart arguments; independent of SDL/GPU for host tests.
use std::ffi::OsString;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(u8)]
pub enum Backend {
    #[default]
    OpenGlEs = 0,
    Vulkan = 1,
}
impl Backend {
    pub fn name(self) -> &'static str {
        match self {
            Self::OpenGlEs => "opengl-es",
            Self::Vulkan => "vulkan",
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::OpenGlEs => "OpenGL ES",
            Self::Vulkan => "Vulkan",
        }
    }
    pub fn from_byte(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::OpenGlEs),
            1 => Some(Self::Vulkan),
            _ => None,
        }
    }
    pub fn from_name(value: &str) -> Option<Self> {
        match value {
            "opengl-es" => Some(Self::OpenGlEs),
            "vulkan" => Some(Self::Vulkan),
            _ => None,
        }
    }
}

pub fn restart_args(
    args: impl IntoIterator<Item = OsString>,
    renderer_change: bool,
    slot: usize,
) -> Vec<OsString> {
    let mut result = Vec::new();
    let mut skip = false;
    for arg in args {
        if skip {
            skip = false;
            continue;
        }
        if arg == "--progress-slot" || (renderer_change && arg == "--renderer") {
            skip = true;
            continue;
        }
        let text = arg.to_string_lossy();
        if text.starts_with("--progress-slot=")
            || (renderer_change && text.starts_with("--renderer="))
        {
            continue;
        }
        result.push(arg);
    }
    result.push("--progress-slot".into());
    result.push((slot + 1).to_string().into());
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn renderer_restart_removes_cli_override_preserves_unicode_paths_and_slot() {
        let args = [
            "C:/游戏/含 空格.swf",
            "--renderer",
            "vulkan",
            "--progress-slot=2",
            "--storage-dir",
            "C:/存档",
        ];
        let result = restart_args(args.map(OsString::from), true, 4);
        assert_eq!(
            result,
            vec![
                OsString::from("C:/游戏/含 空格.swf"),
                "--storage-dir".into(),
                "C:/存档".into(),
                "--progress-slot".into(),
                "5".into()
            ]
        );
        let result = restart_args(
            ["game.swf", "--renderer=vulkan", "--progress-slot", "3"].map(OsString::from),
            false,
            0,
        );
        assert_eq!(
            result,
            vec![
                OsString::from("game.swf"),
                "--renderer=vulkan".into(),
                "--progress-slot".into(),
                "1".into()
            ]
        );
    }
}
