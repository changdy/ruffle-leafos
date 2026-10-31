//! Leaf's Catastrophe five-row keyboard adapted to the existing GL overlay.
//! Layout/navigation derived from Catastrophe 1673e98 (MIT, Kevin Vranken).
//! See vendor/catastrophe.lock.json and licenses/CATASTROPHE-LICENSE.txt.
use std::sync::{Arc, atomic::{AtomicU64, Ordering}};

pub const MAX_BYTES: usize = 16 * 1024;

/// Focus callbacks can run while Player is locked. Only queue a request here;
/// opening the overlay and releasing game inputs belongs to the main loop.
#[derive(Clone, Default)]
pub struct Request(Arc<AtomicU64>);
impl Request {
    pub fn open(&self) { self.0.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |v| Some(v.wrapping_add(2) | 1)).unwrap(); }
    pub fn close(&self) { self.0.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |v| Some(v.wrapping_add(2) & !1)).unwrap(); }
    pub fn get(&self) -> u64 { self.0.load(Ordering::Relaxed) }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Input { Up, Down, Left, Right, Type, Backspace, Delete, Space, Shift,
    Symbols, CursorLeft, CursorRight, Home, End, Newline, Confirm, Cancel, Help }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome { Editing, Confirm, Cancel }

pub struct Key { pub row: usize, pub col: usize, pub label: String, pub rect: (f32,f32,f32,f32) }

pub struct Editor {
    pub original: String,
    pub text: String,
    pub cursor: usize, // UTF-8 boundary; Ruffle selections use UTF-16 instead.
    anchor: usize,
    pub password: bool,
    pub multiline: bool,
    pub row: usize,
    pub col: usize,
    pub shift: bool,
    pub symbols: bool,
    pub help: bool,
}

const ROWS: [&str; 4] = ["1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"];
const SYMBOLS: [&str; 4] = ["!@#$%^&*()", "`~[]\\|{};:", "'\"<>?/+=_", ",.-€£¥¢"];
const LAST_COL: [usize;5] = [10,9,9,8,0];

fn utf8_index(text: &str, utf16: usize) -> usize {
    let mut units=0;
    for (byte,c) in text.char_indices() {
        if units>=utf16 { return byte; }
        units+=c.len_utf16();
    }
    text.len()
}

impl Editor {
    pub fn new(text: String, selection: (usize,usize), password: bool, multiline: bool) -> Option<Self> {
        if text.len()>MAX_BYTES { return None; }
        let anchor=utf8_index(&text,selection.0);let cursor=utf8_index(&text,selection.1);
        Some(Self { original:text.clone(),text,cursor,anchor,password,multiline,
            row:1,col:0,shift:false,symbols:false,help:false })
    }
    fn selection(&self) -> std::ops::Range<usize> { self.cursor.min(self.anchor)..self.cursor.max(self.anchor) }
    pub fn insert(&mut self, text: &str) {
        let text:String=text.chars().filter(|c| !c.is_control() || (self.multiline && *c=='\n')).collect();
        if text.is_empty() { return; }
        let range=self.selection();
        if self.text.len()-range.len()+text.len()>MAX_BYTES { return; }
        self.cursor=range.start+text.len();self.anchor=self.cursor;
        self.text.replace_range(range,&text);
    }
    fn previous(&self) -> usize { self.text[..self.cursor].char_indices().next_back().map(|(i,_)|i).unwrap_or(0) }
    fn next(&self) -> usize { self.cursor+self.text[self.cursor..].chars().next().map(char::len_utf8).unwrap_or(0) }
    fn character(&self,row:usize,col:usize) -> String {
        let c=if self.symbols {SYMBOLS[row]} else {ROWS[row]}.chars().nth(col).unwrap();
        if self.shift { c.to_uppercase().collect() } else { c.to_string() }
    }
    pub fn label(&self,row:usize,col:usize) -> String {
        match (row,col) {
            (0,10)=>super::i18n::t("退格").into(),(2,9)=>super::i18n::t("确认").into(),(3,0)=>if self.shift {"ABC"}else{"abc"}.into(),
            (3,8)=>if self.symbols {"ABC"}else{"#+="}.into(),(4,_)=>super::i18n::t("空格").into(),
            (3,_)=>self.character(row,col-1),_=>self.character(row,col),
        }
    }
    pub fn input(&mut self,input:Input) -> Outcome {
        if input==Input::Cancel { return Outcome::Cancel; }
        if input==Input::Help { self.help=!self.help;return Outcome::Editing; }
        if self.help { if matches!(input,Input::Type|Input::Confirm|Input::Backspace) {self.help=false;}return Outcome::Editing; }
        let input=if input==Input::Type {match (self.row,self.col) {
            (0,10)=>Input::Backspace,(2,9)=>Input::Confirm,(3,0)=>Input::Shift,(3,8)=>Input::Symbols,(4,_)=>Input::Space,
            _=>{self.insert(&self.label(self.row,self.col));return Outcome::Editing;}
        }} else {input};
        match input {
            Input::Up=>{self.row=(self.row+4)%5;self.col=self.col.min(LAST_COL[self.row]);},
            Input::Down=>{self.row=(self.row+1)%5;self.col=self.col.min(LAST_COL[self.row]);},
            Input::Left=>self.col=(self.col+LAST_COL[self.row])%(LAST_COL[self.row]+1),
            Input::Right=>self.col=(self.col+1)%(LAST_COL[self.row]+1),
            Input::Shift=>{self.shift=!self.shift;self.symbols=false;},
            Input::Symbols=>{self.symbols=!self.symbols;self.shift=false;},
            Input::Space=>self.insert(" "),Input::Newline=>{if self.multiline {self.insert("\n");}},
            Input::Backspace|Input::Delete=>{
                let mut range=self.selection();
                if range.is_empty() {if input==Input::Backspace {range.start=self.previous();}else{range.end=self.next();}}
                self.cursor=range.start;self.anchor=self.cursor;self.text.replace_range(range,"");
            },
            Input::CursorLeft=>{self.cursor=if self.selection().is_empty(){self.previous()}else{self.selection().start};self.anchor=self.cursor;},
            Input::CursorRight=>{self.cursor=if self.selection().is_empty(){self.next()}else{self.selection().end};self.anchor=self.cursor;},
            Input::Home=>{self.cursor=0;self.anchor=0;},Input::End=>{self.cursor=self.text.len();self.anchor=self.cursor;},
            Input::Confirm=>return Outcome::Confirm,
            _=>{},
        }
        Outcome::Editing
    }
    /// Bounded preview keeps text textures small and never exposes passwords.
    pub fn preview(&self) -> String {
        let before:Vec<_>=self.text[..self.cursor].chars().rev().take(26).collect();
        let after:Vec<_>=self.text[self.cursor..].chars().take(18).collect();
        let display=|c:char|if self.password {'*'}else if c=='\n' {'↵'}else{c};
        before.into_iter().rev().map(display).chain(['|']).chain(after.into_iter().map(display)).collect()
    }
    pub fn cursor_utf16(&self) -> usize {self.text[..self.cursor].encode_utf16().count()}
    /// Replace only the edited span, preserving existing characters that may
    /// predate a game's restrict filter. Ruffle applies maxChars/restrict/events.
    pub fn replacement(&self) -> (usize,usize,String) {
        let prefix=self.original.chars().zip(self.text.chars()).take_while(|(a,b)|a==b).map(|(c,_)|c.len_utf8()).sum::<usize>();
        let suffix=self.original[prefix..].chars().rev().zip(self.text[prefix..].chars().rev()).take_while(|(a,b)|a==b).map(|(c,_)|c.len_utf8()).sum::<usize>();
        let end=self.original.len()-suffix;
        (self.original[..prefix].encode_utf16().count(),self.original[..end].encode_utf16().count(),self.text[prefix..self.text.len()-suffix].into())
    }
    pub fn keys(&self) -> Vec<Key> {
        let mut keys=Vec::new();let w=70.;let gap=6.;let h=69.;
        for row in 0..5 {
            let widths:Vec<f32>=match row {
                0=>(0..11).map(|i|if i==10 {w*2.+gap}else{w}).collect(),
                1=>vec![w;10],2=>(0..10).map(|i|if i==9 {w*1.5+gap/2.}else{w}).collect(),
                3=>(0..9).map(|i|if i==0||i==8 {w*2.+gap}else{w}).collect(),_=>vec![w*8.+gap*7.],
            };
            let total=widths.iter().sum::<f32>()+gap*(widths.len()-1) as f32;let mut x=(960.-total)/2.;
            for (col,width) in widths.into_iter().enumerate() {
                keys.push(Key {row,col,label:self.label(row,col),rect:(x,204.+row as f32*(h+gap),width,h)});x+=width+gap;
            }
        }
        keys
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn editor(text:&str)->Editor {Editor::new(text.into(),(text.encode_utf16().count(),text.encode_utf16().count()),false,false).unwrap()}
    #[test] fn focus_requests_distinguish_reopening_the_same_field() {let r=Request::default();r.open();let a=r.get();r.open();assert_ne!(a,r.get());assert_eq!(r.get()&1,1);r.close();assert_eq!(r.get()&1,0);}
    #[test] fn unicode_selection_insertion_and_delete_use_character_boundaries() {
        let mut e=Editor::new("a中😀z".into(),(1,4),false,false).unwrap();e.insert("£");assert_eq!(e.text,"a£z");
        assert_eq!(e.replacement(),(1,4,"£".into()));e.input(Input::Backspace);assert_eq!(e.text,"az");e.input(Input::Delete);assert_eq!(e.text,"a");
    }
    #[test] fn unchanged_and_empty_replacements_are_exact() {let mut e=editor("abc");assert_eq!(e.replacement(),(3,3,"".into()));e.input(Input::Backspace);assert_eq!(e.replacement(),(2,3,"".into()));}
    #[test] fn cancelling_and_confirming_are_transactional() {let mut e=editor("old");e.insert("q");assert_eq!(e.input(Input::Cancel),Outcome::Cancel);assert_eq!(e.original,"old");assert_eq!(e.input(Input::Confirm),Outcome::Confirm);}
    #[test] fn leaf_layout_wraps_and_toggles_case_and_symbols() {let mut e=editor("");e.input(Input::Left);assert_eq!(e.col,9);e.input(Input::Type);e.input(Input::Shift);e.input(Input::Type);e.input(Input::Symbols);e.input(Input::Type);assert_eq!(e.text,"pP:");e.input(Input::Down);e.input(Input::Down);assert_eq!((e.row,e.col),(3,8));assert_eq!(e.keys().len(),41);}
    #[test] fn preview_masks_passwords_and_is_bounded() {let e=Editor::new("secret中😀".into(),(9,9),true,false).unwrap();assert_eq!(e.preview(),"********|");assert!(editor(&"a".repeat(2000)).preview().len()<50);}
    #[test] fn multiline_and_input_bounds_preserve_originals() {let mut e=editor("a");e.insert("\n\t");assert_eq!(e.text,"a");e.multiline=true;e.input(Input::Newline);assert_eq!(e.text,"a\n");assert!(Editor::new("a".repeat(MAX_BYTES+1),(0,0),false,false).is_none());let mut e=editor(&"a".repeat(MAX_BYTES));e.insert("q");assert_eq!(e.text.len(),MAX_BYTES);}
    #[test] fn cursor_and_replacement_handle_surrogates_and_common_suffixes() {let mut e=editor("中😀尾");e.input(Input::CursorLeft);e.insert("x");assert_eq!(e.replacement(),(3,3,"x".into()));e.input(Input::CursorLeft);e.input(Input::CursorLeft);assert_eq!(e.cursor_utf16(),1);}
}
