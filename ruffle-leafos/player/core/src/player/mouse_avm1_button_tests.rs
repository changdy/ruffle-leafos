use super::compatibility_tests::*;
use super::Player;
use crate::display_object::{ButtonState, TDisplayObject, TDisplayObjectContainer};
use crate::limits::ExecutionLimit;

fn button_probe() -> std::sync::Arc<std::sync::Mutex<Player>> {
    let (player, _) = player_for_swf(include_bytes!("tests/button-drag.swf"));
    {
        let mut player = player.lock().unwrap();
        player.preload(&mut ExecutionLimit::none());
        player.run_frame();
        player.run_frame();
        for depth in 1..=5 {
            assert_eq!(state(&mut player, depth), ButtonState::Up);
        }
    }
    player
}

fn state(player: &mut Player, depth: i32) -> ButtonState {
    player.mutate_with_update_context(|context| {
        context.stage.root_clip().unwrap().as_container().unwrap()
            .child_by_depth(depth).unwrap().as_avm1_button().unwrap().state().unwrap()
    })
}

#[test]
fn avm1_unpressed_push_button_does_not_latch_its_highlight_when_dragged_across() {
    let player = button_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    press(&mut player, 150.0, 150.0);
    move_to(&mut player, 350.0, 150.0);
    assert_eq!(state(&mut player, 2), ButtonState::Up);
    move_to(&mut player, 900.0, 600.0);
    release(&mut player, 900.0, 600.0);
    assert_eq!(state(&mut player, 2), ButtonState::Up);
}

#[test]
fn avm1_pressed_push_button_recovers_its_down_state_after_dragging_back_inside() {
    let player = button_probe();
    let mut player = player.lock().unwrap();
    move_to(&mut player, 150.0, 150.0);
    assert_eq!(state(&mut player, 1), ButtonState::Over);
    press(&mut player, 150.0, 150.0);
    assert_eq!(state(&mut player, 1), ButtonState::Down);
    move_to(&mut player, 900.0, 600.0);
    assert_eq!(state(&mut player, 1), ButtonState::Over);
    move_to(&mut player, 150.0, 150.0);
    assert_eq!(state(&mut player, 1), ButtonState::Down);
    release(&mut player, 150.0, 150.0);
    assert_eq!(state(&mut player, 1), ButtonState::Over);
}

#[test]
fn avm1_swf_menu_button_still_tracks_drags_started_on_another_button() {
    let player = button_probe();
    let mut player = player.lock().unwrap();
    press(&mut player, 150.0, 150.0);
    move_to(&mut player, 550.0, 150.0);
    assert_eq!(state(&mut player, 3), ButtonState::Down);
}

#[test]
fn avm1_dynamic_track_as_menu_enables_cross_button_drag_tracking() {
    let player = button_probe();
    let mut player = player.lock().unwrap();
    press(&mut player, 150.0, 150.0);
    move_to(&mut player, 750.0, 150.0);
    assert_eq!(state(&mut player, 4), ButtonState::Down);
}

#[test]
fn avm1_dynamic_track_as_menu_false_overrides_the_swf_menu_flag() {
    let player = button_probe();
    let mut player = player.lock().unwrap();
    press(&mut player, 150.0, 150.0);
    move_to(&mut player, 150.0, 350.0);
    assert_eq!(state(&mut player, 5), ButtonState::Up);
    move_to(&mut player, 900.0, 600.0);
    assert_eq!(state(&mut player, 5), ButtonState::Up);
}
