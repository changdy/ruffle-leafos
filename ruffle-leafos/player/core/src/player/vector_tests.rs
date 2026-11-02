//! Check parameterized Vector semantics with the optimizer enabled and disabled.
use super::compatibility_tests::TraceLog;
use super::*;

fn run_vector_probe(optimized: bool) -> Vec<String> {
    let movie = SwfMovie::from_data(
        include_bytes!("tests/vector-native.swf"),
        "file:///vector-native.swf".to_string(),
        None,
        None,
    )
    .unwrap();
    let trace = TraceLog::default();
    let player = PlayerBuilder::new()
        .with_movie(movie)
        .with_log(trace.clone())
        .with_autoplay(true)
        .with_avm2_optimizer_enabled(optimized)
        .build();
    {
        let mut player = player.lock().unwrap();
        assert!(player.preload(&mut ExecutionLimit::none()));
        player.run_frame();
        player.run_frame();
    }
    trace.take()
}

#[test]
fn vector_native_calls_preserve_typed_length_fixed_index_and_errors() {
    let optimized = run_vector_probe(true);
    let unoptimized = run_vector_probe(false);
    assert_eq!(optimized, unoptimized);
    assert_eq!(
        optimized.last().map(String::as_str),
        Some("VECTOR_PROBE_DONE"),
        "the probe must complete all checks: {optimized:?}"
    );
    assert_eq!(
        optimized.iter().filter(|line| line.starts_with("VECTOR_PASS:")).count(),
        29,
        "the complete probe must run without a swallowed ActionScript exception: {optimized:?}"
    );
    assert!(optimized.iter().all(|line| line.starts_with("VECTOR_PASS:") || line == "VECTOR_PROBE_DONE"));
}
