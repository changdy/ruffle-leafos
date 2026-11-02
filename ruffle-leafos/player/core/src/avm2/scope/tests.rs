//! Real AVM2 scope-resolution regressions using null platform backends.

use crate::backend::log::LogBackend;
use crate::limits::ExecutionLimit;
use crate::tag_utils::SwfMovie;
use crate::PlayerBuilder;
use std::cell::RefCell;
use std::rc::Rc;

#[derive(Clone, Default)]
struct TraceLog(Rc<RefCell<Vec<String>>>);

impl LogBackend for TraceLog {
    fn avm_trace(&self, message: &str) {
        self.0.borrow_mut().push(message.to_owned());
    }

    fn avm_warning(&self, _message: &str) {}
}

fn assert_lexical_scope_wins(bytes: &[u8]) {
    // Upstream scopes_dont_cache fixtures use runtime MultinameL names.
    // Therefore the verifier cannot fold the property lookups into static
    // scope accesses and accidentally hide a ScopeChain cache error.
    let movie = SwfMovie::from_data(
        bytes, "file:///scope-lookup-test.swf".to_owned(), None, None,
    ).unwrap();
    let log = TraceLog::default();
    let player = PlayerBuilder::new()
        .with_movie(movie)
        .with_log(log.clone())
        .with_autoplay(true)
        .build();
    let mut player = player.lock().unwrap();
    assert!(player.preload(&mut ExecutionLimit::none()));
    player.run_frame();
    player.run_frame();
    assert_eq!(*log.0.borrow(), ["inner-value"]);
}

#[test]
fn scopes_dont_cache_inner_then_outer_namespace_lookups() {
    assert_lexical_scope_wins(include_bytes!(
        "fixtures/scopes-dont-cache-order-1.swf"
    ));
}

#[test]
fn scopes_dont_cache_outer_then_inner_namespace_lookups() {
    assert_lexical_scope_wins(include_bytes!(
        "fixtures/scopes-dont-cache-order-2.swf"
    ));
}
