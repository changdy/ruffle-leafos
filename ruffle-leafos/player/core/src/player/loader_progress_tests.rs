use super::compatibility_tests::player_for_swf;

#[test]
fn root_progress_reaches_first_frame_listener_after_init() {
    let (player, traces) = player_for_swf(include_bytes!("tests/root-progress.swf"));
    let mut player = player.lock().unwrap();
    player.run_frame();
    player.run_frame();
    assert_eq!(traces.take(), ["constructor", "exitFrame", "init", "progress: complete", "complete", "enterFrame", "exitFrame"]);
}

#[test]
fn root_progress_goto_does_not_repeat_completion() {
    let (player, traces) = player_for_swf(include_bytes!("tests/root-progress-goto.swf"));
    let mut player = player.lock().unwrap();
    player.run_frame();
    player.run_frame();
    assert_eq!(traces.take(), ["constructor", "exitFrame", "init", "progress: complete", "gotoAndStop(1)", "exitFrame", "gotoAndStop(1) done", "complete", "enterFrame", "exitFrame"]);
}
