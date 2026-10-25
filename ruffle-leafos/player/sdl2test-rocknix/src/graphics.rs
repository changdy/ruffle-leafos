//! Renderer availability shared by the menu and host transition tests.
//! Driver/window support and an implemented movie backend are separate facts.

#[derive(Clone, Debug, Default, PartialEq)]
pub enum VulkanWindow {
    #[default]
    Unchecked,
    Available,
    Unavailable(String),
}

impl VulkanWindow {
    pub fn message(&self) -> &'static str {
        match self {
            Self::Unchecked => "尚未检测 Vulkan，继续使用 OpenGL ES",
            Self::Unavailable(_) => "当前系统无法显示 Vulkan 画面，继续使用 OpenGL ES",
            Self::Available => "此版本尚未接入 Vulkan，继续使用 OpenGL ES",
        }
    }

    pub fn details(&self) -> [&'static str; 2] {
        [
            "此版本已接入 OpenGL ES；Vulkan 渲染尚未接入。",
            match self {
                Self::Unchecked => "系统 Vulkan 窗口支持：尚未检测。",
                Self::Available => "系统 Vulkan 窗口支持：检测通过，仍需接入并验证渲染。",
                Self::Unavailable(_) => "系统 Vulkan 窗口支持：检测未通过。",
            },
        ]
    }
}
