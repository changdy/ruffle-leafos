//! Public Player/StorageBackend integration tests with the old fork's genuine
//! AMF0 writer output; no private VM shortcuts or new-writer seed generation.
use ruffle_core::backend::log::LogBackend;
use ruffle_core::backend::storage::StorageBackend;
use ruffle_core::limits::ExecutionLimit;
use ruffle_core::tag_utils::SwfMovie;
use ruffle_core::{Player, PlayerBuilder};
use std::cell::RefCell;
use std::rc::Rc;
use std::sync::{Arc, Mutex};

// Both old and new AVM1 getLocal derive this exact key from file:///... and
// localPath="/". Seed only the old key: computing it from the new VM would
// conceal an identity change during migration.
const STORAGE_KEY: &str = "localhost//leaf-shared-avm1";
const LEGACY_BYTES: &[u8] = include_bytes!("fixtures/shared-object-amf0-legacy.sol");

#[derive(Clone, Default)]
struct TraceLog(Rc<RefCell<Vec<String>>>);

impl LogBackend for TraceLog {
    fn avm_trace(&self, message: &str) {
        self.0.borrow_mut().push(message.into());
    }

    fn avm_warning(&self, message: &str) {
        self.0.borrow_mut().push(format!("WARNING: {message}"));
    }
}

impl TraceLog {
    fn take(&self) -> Vec<String> {
        std::mem::take(&mut *self.0.borrow_mut())
    }
}

struct StorageState {
    bytes: Vec<u8>,
    requested_keys: Vec<String>,
    writes: usize,
    rejected_writes: usize,
    reject_writes: bool,
}

#[derive(Clone)]
struct ObservableStorage(Rc<RefCell<StorageState>>);

impl ObservableStorage {
    fn legacy(reject_writes: bool) -> Self {
        Self(Rc::new(RefCell::new(StorageState {
            bytes: LEGACY_BYTES.to_vec(), requested_keys: Vec::new(),
            writes: 0, rejected_writes: 0, reject_writes,
        })))
    }
}

impl StorageBackend for ObservableStorage {
    fn get(&self, name: &str) -> Option<Vec<u8>> {
        let mut state = self.0.borrow_mut();
        state.requested_keys.push(name.into());
        (name == STORAGE_KEY).then(|| state.bytes.clone())
    }

    fn put(&mut self, name: &str, value: &[u8]) -> bool {
        assert_eq!(name, STORAGE_KEY, "the AVM1 storage identity changed");
        let mut state = self.0.borrow_mut();
        state.writes += 1;
        if state.reject_writes {
            state.rejected_writes += 1;
            return false;
        }
        state.bytes = value.to_vec();
        true
    }

    fn remove_key(&mut self, name: &str) {
        panic!("AVM1 migration probe unexpectedly removed {name}");
    }
}

fn start(storage: ObservableStorage) -> (Arc<Mutex<Player>>, TraceLog) {
    let movie = SwfMovie::from_data(
        include_bytes!("fixtures/shared-object-avm1.swf"),
        "file:///compatibility-test.swf".into(), None, None,
    ).unwrap();
    let trace = TraceLog::default();
    let player = PlayerBuilder::new()
        .with_movie(movie).with_storage(Box::new(storage))
        .with_log(trace.clone()).with_autoplay(true).build();
    {
        let mut player = player.lock().unwrap();
        player.preload(&mut ExecutionLimit::none());
        player.run_frame();
        player.run_frame();
    }
    (player, trace)
}

fn assert_old_data(trace: &TraceLog) {
    assert_eq!(trace.take(), [
        "AMF0_COMPAT legacy-avm1-v0.8.3|旧档✓|41|1.25|true|1,two,false|3,nested",
        "AMF0_COMPAT DIRTY",
    ]);
}

fn assert_reopened_data(storage: ObservableStorage) {
    let (_player, trace) = start(storage);
    assert_eq!(trace.take(), [
        "AMF0_COMPAT migrated-avm1|旧档✓|42|1.25|true|1,two,false,new|4,persisted",
        "AMF0_COMPAT DIRTY",
    ]);
}

#[test]
fn avm1_legacy_amf0_loads_and_checked_flush_survives_reopen() {
    let storage = ObservableStorage::legacy(false);
    let (player, trace) = start(storage.clone());
    assert_old_data(&trace);
    assert_eq!(storage.0.borrow().requested_keys, [STORAGE_KEY]);
    assert!(player.lock().unwrap().flush_shared_objects_checked());
    {
        let state = storage.0.borrow();
        assert_eq!((state.writes, state.rejected_writes), (1, 0));
        assert_ne!(state.bytes, LEGACY_BYTES);
    }
    assert_reopened_data(storage);
}

#[test]
fn avm1_checked_flush_reports_rejected_write_and_retry_preserves_progress() {
    let storage = ObservableStorage::legacy(true);
    let (player, trace) = start(storage.clone());
    assert_old_data(&trace);
    assert!(!player.lock().unwrap().flush_shared_objects_checked());
    {
        let state = storage.0.borrow();
        assert_eq!((state.writes, state.rejected_writes), (1, 1));
        assert_eq!(state.bytes, LEGACY_BYTES);
    }
    storage.0.borrow_mut().reject_writes = false;
    assert!(player.lock().unwrap().flush_shared_objects_checked());
    {
        let state = storage.0.borrow();
        assert_eq!((state.writes, state.rejected_writes), (2, 1));
    }
    assert_reopened_data(storage);
}
