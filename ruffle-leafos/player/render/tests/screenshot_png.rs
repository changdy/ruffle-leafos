// Build the frontend's actual encoder in this small, locked-dependency harness
// so pixel/stream regressions do not require SDL or a GPU.
#[path = "../../sdl2test-rocknix/src/screenshot_png.rs"]
mod screenshot_png;

use std::io::{self, Cursor, Write};

#[test]
fn rgb_roundtrip_flips_odd_height_rows_and_discards_only_alpha() {
    let bottom_up = [
        1,2,3,0, 4,5,6,120, 7,8,9,255,
        10,11,12,4, 13,14,15,5, 16,17,18,6,
        19,20,21,0, 22,23,24,0, 25,26,27,0,
    ];
    let mut encoded = Vec::new();
    screenshot_png::write_rgb(&mut encoded,3,3,&bottom_up).unwrap();
    let mut reader = png::Decoder::new(Cursor::new(encoded)).read_info().unwrap();
    let mut decoded = vec![0;reader.output_buffer_size().unwrap()];
    let info = reader.next_frame(&mut decoded).unwrap();
    assert_eq!((info.width,info.height,info.color_type),(3,3,png::ColorType::Rgb));
    assert_eq!(decoded,[19,20,21,22,23,24,25,26,27,10,11,12,13,14,15,16,17,18,1,2,3,4,5,6,7,8,9]);
}

#[test]
fn noisy_full_screen_is_written_in_bounded_chunks() {
    struct Bounded { bytes: usize, max_write: usize }
    impl Write for Bounded {
        fn write(&mut self, b:&[u8])->io::Result<usize> {
            assert!(b.len()<=32*1024,"unbounded compressed output write");
            self.bytes+=b.len();self.max_write=self.max_write.max(b.len());Ok(b.len())
        }
        fn flush(&mut self)->io::Result<()> {Ok(())}
    }
    let mut pixels=vec![0;960*720*4];let mut seed=0x243f6a88u32;
    for pixel in pixels.chunks_exact_mut(4) {
        seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
        pixel.copy_from_slice(&seed.to_le_bytes());
    }
    let mut output=Bounded{bytes:0,max_write:0};
    screenshot_png::write_rgb(&mut output,960,720,&pixels).unwrap();
    assert!(output.bytes>1024*1024);assert_eq!(output.max_write,32*1024);
}

#[test]
fn output_failure_and_invalid_input_return_errors() {
    struct FailsAfter(usize);
    impl Write for FailsAfter {
        fn write(&mut self,b:&[u8])->io::Result<usize> {
            if self.0==0 {return Err(io::Error::new(io::ErrorKind::StorageFull,"injected full card"));}
            let n=b.len().min(self.0);self.0-=n;Ok(n)
        }
        fn flush(&mut self)->io::Result<()> {Ok(())}
    }
    let error=screenshot_png::write_rgb(FailsAfter(45),1,1,&[1,2,3,0]).unwrap_err();
    assert!(error.to_string().contains("injected full card"));
    for (w,h,bytes) in [(0,1,&[][..]),(1,0,&[][..]),(1,1,&[0;3][..]),(u32::MAX,u32::MAX,&[][..])] {
        assert!(screenshot_png::write_rgb(io::sink(),w,h,bytes).is_err());
    }
}
