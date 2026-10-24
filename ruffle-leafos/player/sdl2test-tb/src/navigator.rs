use ruffle_frontend_utils::backends::navigator::NavigatorInterface;
use std::fs::File;
use std::path::Path;
use url::Url;

#[derive(Clone)]
pub struct LinuxNavigatorInterface;

// TODO: Prompt the user for these things!
impl NavigatorInterface for LinuxNavigatorInterface {
    fn navigate_to_website(&self, _url: Url) {
        // TODO: Implement `ask` behavior!
        //let _ = webbrowser::open(url.as_ref());
	    println!("打开外部网址");
    }

    async fn open_file(&self, path: &Path) -> std::io::Result<File> {
        //println!("openfile");
        File::open(path)
    }

    async fn confirm_socket(&self, _host: &str, _port: u16) -> bool {
        true
    }
}
