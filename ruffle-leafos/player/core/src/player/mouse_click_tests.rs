use super::compatibility_tests::*;

#[test]
fn avm2_release_over_another_sprite_does_not_click_the_pressed_sprite() {
    let (player, trace) = mouse_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    press(&mut player, 150.0, 150.0);
    move_to(&mut player, 350.0, 150.0);
    release(&mut player, 350.0, 150.0);
    let events = trace.take();
    assert!(events.iter().any(|e| e == "A mouseDown"), "{events:?}");
    assert!(events.iter().any(|e| e == "B mouseUp"), "{events:?}");
    assert!(events.iter().any(|e| e == "A releaseOutside"), "{events:?}");
    assert!(!events.iter().any(|e| e.ends_with(" click")), "{events:?}");
}

#[test]
fn avm2_release_inside_the_pressed_sprite_still_clicks() {
    let (player, trace) = mouse_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    press(&mut player, 150.0, 150.0);
    release(&mut player, 150.0, 150.0);
    let events = trace.take();
    assert_eq!(events.iter().filter(|e| *e == "A click").count(), 1, "{events:?}");
    assert!(!events.iter().any(|e| e.ends_with(" releaseOutside")), "{events:?}");
}
