//! Execute upstream Flash-reference traces through a real Player and null backends.

use crate::backend::log::LogBackend;
use crate::backend::navigator::{NullExecutor, NullNavigatorBackend};
use crate::limits::ExecutionLimit;
use crate::tag_utils::SwfMovie;
use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::PlayerBuilder;

#[derive(Clone, Default)]
struct TraceLog(Rc<RefCell<Vec<String>>>);

impl LogBackend for TraceLog {
    fn avm_trace(&self, message: &str) {
        self.0.borrow_mut().push(message.to_string());
    }

    fn avm_warning(&self, _message: &str) {}
}

// Embed the only external resource so these tests also run from a standalone
// ARM64 test executable, without access to the build machine's source tree.
struct ChildMovieDir(PathBuf);

impl ChildMovieDir {
    fn new() -> Self {
        static NEXT_ID: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "ruffle-avm1-onload-{}-{}",
            std::process::id(),
            NEXT_ID.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path).unwrap();
        std::fs::write(
            path.join("child.swf"),
            include_bytes!("onload_fixtures/issue_2870/child.swf"),
        )
        .unwrap();
        Self(path)
    }
}

impl Drop for ChildMovieDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(self.0.join("child.swf"));
        let _ = std::fs::remove_dir(&self.0);
    }
}

fn assert_trace(bytes: &[u8], expected: &str, frames: usize, child: bool) {
    let mut executor = NullExecutor::new();
    let resource_dir = child.then(ChildMovieDir::new);
    let mut builder = PlayerBuilder::new();
    let url = if let Some(dir) = &resource_dir {
        builder = builder
            .with_navigator(NullNavigatorBackend::with_base_path(&dir.0, &executor).unwrap());
        url::Url::from_file_path(dir.0.join("test.swf"))
            .unwrap()
            .to_string()
    } else {
        "file:///avm1-onload/test.swf".to_string()
    };
    let log = TraceLog::default();
    let movie = SwfMovie::from_data(bytes, url, None).unwrap();
    let player = builder
        .with_movie(movie)
        .with_log(log.clone())
        .with_autoplay(true)
        .build();
    assert!(player.lock().unwrap().preload(&mut ExecutionLimit::none()));
    for _ in 0..frames {
        player.lock().unwrap().run_frame();
        executor.run();
    }
    let actual = log.0.borrow().join("\n");
    assert_eq!(actual, expected.trim_end_matches('\n'));
}

#[test]
fn avm1_onload_root_handler_defined_by_first_frame_runs_once() {
    assert_trace(
        include_bytes!("onload_fixtures/root_onload/test.swf"),
        include_str!("onload_fixtures/root_onload/output.txt"),
        30,
        false,
    );
}

#[test]
fn avm1_onload_root_and_child_construction_order_matches_flash() {
    assert_trace(
        include_bytes!("onload_fixtures/swfmill_root_onload/test.swf"),
        include_str!("onload_fixtures/swfmill_root_onload/output.txt"),
        30,
        false,
    );
}

#[test]
fn avm1_onload_action_and_enter_frame_order_matches_flash() {
    assert_trace(
        include_bytes!("onload_fixtures/action_execution_order_test4/test.swf"),
        include_str!("onload_fixtures/action_execution_order_test4/output.txt"),
        30,
        false,
    );
}

#[test]
fn avm1_onload_load_movie_keeps_clip_load_before_loaded_frame() {
    assert_trace(
        include_bytes!("onload_fixtures/issue_2870/test.swf"),
        include_str!("onload_fixtures/issue_2870/output.txt"),
        10,
        true,
    );
}
