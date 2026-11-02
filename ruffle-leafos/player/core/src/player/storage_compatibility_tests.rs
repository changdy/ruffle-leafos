//! The old fork's pinned AMF writer supplies the seed bytes. All reads,
//! mutations and subsequent writes run through the real AVM2 SharedObject API.
use super::compatibility_tests::TraceLog;
use super::*;
use std::cell::RefCell;

const STORAGE_KEY: &str = "localhost//leaf-shared-compat";
const LEGACY_BYTES: &[u8] = include_bytes!("tests/shared-object-legacy.sol");

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
            bytes: LEGACY_BYTES.to_vec(),
            requested_keys: Vec::new(),
            writes: 0,
            rejected_writes: 0,
            reject_writes,
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
        assert_eq!(name, STORAGE_KEY, "storage identity changed during migration");
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
        panic!("compatibility fixture unexpectedly removed {name}");
    }
}

fn start(storage: ObservableStorage) -> (Arc<Mutex<Player>>, TraceLog) {
    let movie = SwfMovie::from_data(
        include_bytes!("tests/shared-object-compat.swf"),
        "file:///compatibility-test.swf".into(),
        None,
        None,
    )
    .unwrap();
    let trace = TraceLog::default();
    let player = PlayerBuilder::new()
        .with_movie(movie)
        .with_storage(Box::new(storage))
        .with_log(trace.clone())
        .with_autoplay(true)
        .build();
    {
        let mut player = player.lock().unwrap();
        player.preload(&mut ExecutionLimit::none());
        player.run_frame();
        player.run_frame();
    }
    (player, trace)
}

fn assert_legacy_read(trace: &TraceLog) {
    assert_eq!(
        trace.take(),
        [
            "SHARED_COMPAT legacy-v0.8.3|旧档✓|41|1.25|true|1,two,false|3,nested",
            "SHARED_COMPAT DIRTY",
        ]
    );
}

fn assert_reopen_reads_updated_data(storage: ObservableStorage) {
    let (_reopened, trace) = start(storage);
    assert_eq!(
        trace.take(),
        [
            "SHARED_COMPAT migrated|旧档✓|42|1.25|true|1,two,false,new|4,persisted",
            "SHARED_COMPAT DIRTY",
        ]
    );
}

#[test]
fn shared_object_legacy_amf3_loads_and_checked_flush_survives_reopen() {
    let storage = ObservableStorage::legacy(false);
    let (player, trace) = start(storage.clone());
    assert_legacy_read(&trace);
    assert_eq!(storage.0.borrow().requested_keys, [STORAGE_KEY]);
    assert!(player.lock().unwrap().flush_shared_objects_checked());
    {
        let state = storage.0.borrow();
        assert_eq!((state.writes, state.rejected_writes), (1, 0));
        assert_ne!(state.bytes, LEGACY_BYTES);
    }
    assert_reopen_reads_updated_data(storage);
}

#[test]
fn shared_object_checked_flush_reports_failed_storage_and_retry_keeps_old_bytes() {
    let storage = ObservableStorage::legacy(true);
    let (player, trace) = start(storage.clone());
    assert_legacy_read(&trace);
    assert!(!player.lock().unwrap().flush_shared_objects_checked());
    {
        let state = storage.0.borrow();
        assert_eq!((state.writes, state.rejected_writes), (1, 1));
        assert_eq!(state.bytes, LEGACY_BYTES, "a rejected write must preserve the old save");
    }
    storage.0.borrow_mut().reject_writes = false;
    assert!(player.lock().unwrap().flush_shared_objects_checked());
    {
        let state = storage.0.borrow();
        assert_eq!((state.writes, state.rejected_writes), (2, 1));
    }
    assert_reopen_reads_updated_data(storage);
}
