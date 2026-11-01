use super::compatibility_tests::*;

fn hover_events(events: Vec<String>) -> Vec<String> {
    events.into_iter().filter(|e| e.ends_with(" rollOver") || e.ends_with(" rollOut")
        || e.ends_with(" mouseOver") || e.ends_with(" mouseOut")).collect()
}

#[test]
fn avm2_drag_out_and_back_dispatches_each_roll_event_once() {
    let (player, trace) = mouse_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    press(&mut player, 150.0, 150.0);
    trace.take();
    move_to(&mut player, 350.0, 150.0);
    move_to(&mut player, 150.0, 150.0);
    assert_eq!(hover_events(trace.take()), [
        "A mouseOut", "A rollOut", "B rollOver", "B mouseOver",
        "B mouseOut", "B rollOut", "A rollOver", "A mouseOver",
    ]);
    release(&mut player, 150.0, 150.0);
    assert!(hover_events(trace.take()).is_empty());
}

#[test]
fn avm2_plain_hover_still_dispatches_each_roll_event_once() {
    let (player, trace) = mouse_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    move_to(&mut player, 350.0, 150.0);
    assert_eq!(hover_events(trace.take()), [
        "A rollOver", "A mouseOver", "A mouseOut", "A rollOut", "B rollOver", "B mouseOver",
    ]);
}

#[test]
fn avm2_release_outside_does_not_repeat_rollover_for_the_hovered_sprite() {
    let (player, trace) = mouse_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    press(&mut player, 150.0, 150.0);
    move_to(&mut player, 350.0, 150.0);
    trace.take();
    release(&mut player, 350.0, 150.0);
    let events = trace.take();
    assert!(events.iter().any(|e| e == "A releaseOutside"), "{events:?}");
    assert!(hover_events(events.clone()).is_empty(), "{events:?}");
}
