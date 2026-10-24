//! Hold-to-repeat input with monotonic deadlines and shared input ownership.
use super::{profiles::Binding, Action, InputLedger, Source, Transition};
use std::{collections::BTreeMap, time::Duration};

#[derive(Clone,Copy,Debug,Default,PartialEq,Eq)]
pub enum Rate { #[default] Off, Slow, Normal, Fast }
impl Rate {
    pub fn hz(self) -> u8 {match self {Self::Off=>0,Self::Slow=>5,Self::Normal=>10,Self::Fast=>15}}
    pub fn parse(text: &str) -> Option<Self> {match text {"0"=>Some(Self::Off),"5"=>Some(Self::Slow),"10"=>Some(Self::Normal),"15"=>Some(Self::Fast),_=>None}}
    pub fn next(self) -> Self {match self {Self::Off=>Self::Slow,Self::Slow=>Self::Normal,Self::Normal=>Self::Fast,Self::Fast=>Self::Off}}
    pub fn label(self) -> &'static str {match self {Self::Off=>"连发关闭",Self::Slow=>"慢速连发 5次/秒",Self::Normal=>"标准连发 10次/秒",Self::Fast=>"快速连发 15次/秒"}}
    pub fn badge(self) -> String {if self==Self::Off {String::new()}else{format!(" · 连{}",self.hz())}}
    fn half_period(self) -> Duration {Duration::from_micros(500_000/u64::from(self.hz().max(1)))}
}
pub fn supported(binding: Binding) -> bool {!binding.actions().is_empty()}

struct Held { actions:Vec<Action>, half:Duration, next:Duration, down:bool }
#[derive(Default)]
pub struct Repeater { held:BTreeMap<Source,Held> }
impl Repeater {
    pub fn press(&mut self,source:Source,binding:Binding,rate:Rate,now:Duration,ledger:&mut InputLedger) -> Vec<Transition> {
        let actions=binding.actions();
        if rate!=Rate::Off && !actions.is_empty() {
            let half=rate.half_period();
            self.held.entry(source).or_insert(Held{actions:actions.clone(),half,next:now+half,down:true});
        }
        ledger.press_many(source,&actions)
    }
    pub fn release(&mut self,source:Source,ledger:&mut InputLedger) -> Vec<Transition> {
        self.held.remove(&source);ledger.release(source)
    }
    pub fn update(&mut self,now:Duration,ledger:&mut InputLedger) -> Vec<Transition> {
        let mut result=Vec::new();
        for (&source,held) in &mut self.held {
            if now<held.next {continue;}
            held.down=!held.down;
            result.extend(if held.down {ledger.press_many(source,&held.actions)}else{ledger.release(source)});
            // Never catch up missed pulses in a burst after a slow Flash frame.
            held.next=if now.saturating_sub(held.next)>=held.half {now+held.half}else{held.next+held.half};
        }
        result
    }
    pub fn clear(&mut self,ledger:&mut InputLedger) -> Vec<Transition> {
        let mut result=Vec::new();
        for source in std::mem::take(&mut self.held).into_keys() {result.extend(ledger.release(source));}
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn ms(n:u64)->Duration {Duration::from_millis(n)}
    #[test]
    fn only_enabled_held_buttons_repeat_and_release_stops_them() {
        let mut repeat=Repeater::default();let mut ledger=InputLedger::default();
        let a=Source::Controller(0);let b=Source::Controller(1);
        repeat.press(a,Binding::Key(106),Rate::Normal,ms(0),&mut ledger);
        repeat.press(b,Binding::Key(107),Rate::Off,ms(0),&mut ledger);
        assert!(repeat.update(ms(49),&mut ledger).is_empty());
        assert_eq!(repeat.update(ms(50),&mut ledger),vec![Transition{action:Action::Key(106),down:false}]);
        assert!(ledger.is_down(Action::Key(107)));
        assert_eq!(repeat.update(ms(100),&mut ledger),vec![Transition{action:Action::Key(106),down:true}]);
        repeat.release(a,&mut ledger);assert!(repeat.update(ms(200),&mut ledger).is_empty());
    }
    #[test]
    fn repeat_respects_shared_modifiers_and_releases_combos_in_reverse_order() {
        let mut repeat=Repeater::default();let mut ledger=InputLedger::default();
        ledger.press(Source::Controller(1),Action::Key(1073742049));
        repeat.press(Source::Controller(0),Binding::Combo([1073742049,9,0,0],2),Rate::Slow,ms(0),&mut ledger);
        assert_eq!(repeat.update(ms(100),&mut ledger),vec![Transition{action:Action::Key(9),down:false}]);
        assert!(ledger.is_down(Action::Key(1073742049)));
        repeat.update(ms(200),&mut ledger);repeat.clear(&mut ledger);
        assert!(repeat.update(ms(1000),&mut ledger).is_empty());assert!(ledger.is_down(Action::Key(1073742049)));
    }
    #[test]
    fn rates_follow_elapsed_time_and_stalls_never_generate_a_burst() {
        for rate in [Rate::Slow,Rate::Normal,Rate::Fast] {
            let mut repeat=Repeater::default();let mut ledger=InputLedger::default();
            repeat.press(Source::Controller(0),Binding::LeftClick,rate,ms(0),&mut ledger);
            let mut presses=0;
            for t in 1..=1000 {presses+=repeat.update(ms(t),&mut ledger).iter().filter(|e|e.down).count();}
            assert_eq!(presses,usize::from(rate.hz()));
            assert!(repeat.update(ms(10000),&mut ledger).len()<=1);
            assert!(repeat.update(ms(10000),&mut ledger).is_empty());
            repeat.clear(&mut ledger);assert!(!ledger.is_down(Action::MouseLeft));
        }
    }
}
