//! A small responsive loading screen while the archive worker prepares a game.
use ab_glyph::{point, Font, FontArc, PxScale, ScaleFont};
use sdl2::{event::Event, pixels::{Color,PixelFormatEnum}, rect::Rect};
use std::{path::PathBuf, sync::mpsc, time::{Duration,Instant}};

fn label(font:&FontArc,text:&str,size:f32)->(Vec<u8>,u32,u32) {
    let scaled=font.as_scaled(PxScale::from(size));
    let width=(text.chars().map(|c|scaled.h_advance(font.glyph_id(c))).sum::<f32>().ceil() as u32+8).max(1);
    let height=(scaled.height().ceil() as u32+8).max(1);
    let mut rgba=vec![0u8;(width*height*4) as usize];let mut x=4.0;
    for ch in text.chars() {
        let glyph=font.glyph_id(ch).with_scale_and_position(size,point(x,scaled.ascent()+4.0));
        if let Some(outline)=font.outline_glyph(glyph) {
            let bounds=outline.px_bounds();outline.draw(|gx,gy,coverage| {
                let px=gx as i32+bounds.min.x as i32;let py=gy as i32+bounds.min.y as i32;
                if px>=0 && py>=0 && px<width as i32 && py<height as i32 {
                    let i=((py as u32*width+px as u32)*4) as usize;
                    rgba[i..i+3].copy_from_slice(&[239,244,252]);rgba[i+3]=(coverage*255.0) as u8;
                }
            });
        }
        x+=scaled.h_advance(font.glyph_id(ch));
    }
    (rgba,width,height)
}

pub fn prepare(zip_path:PathBuf,cache:PathBuf,fullscreen:bool)->Result<PathBuf,String> {
    let sdl=sdl2::init()?;let video=sdl.video()?;
    let mut window=video.window("Ruffle Flash",960,720).position_centered().build().map_err(|e|e.to_string())?;
    if fullscreen {window.set_fullscreen(sdl2::video::FullscreenType::Desktop)?;}
    let mut canvas=window.into_canvas().accelerated().present_vsync().build().map_err(|e|e.to_string())?;
    canvas.set_logical_size(960,720).map_err(|e|e.to_string())?;
    let font_path=std::env::var_os("RUFFLE_FONT_FILE").or_else(||std::env::var_os("RUFFLE_MENU_FONT")).unwrap_or_else(||"res/menu-font.ttf".into());
    let font=FontArc::try_from_vec(std::fs::read(font_path).map_err(|e|format!("loading font: {e}"))?).map_err(|_|"Invalid loading font".to_string())?;
    let (pixels,w,h)=label(&font,&crate::tr!("正在加载游戏…", "Loading game…"),38.0);
    let creator=canvas.texture_creator();
    let mut texture=creator.create_texture_streaming(PixelFormatEnum::RGBA32,w,h).map_err(|e|e.to_string())?;
    texture.set_blend_mode(sdl2::render::BlendMode::Blend);
    texture.update(None,&pixels,(w*4) as usize).map_err(|e|e.to_string())?;
    canvas.set_draw_color(Color::RGB(17,24,37));canvas.clear();
    canvas.set_draw_color(Color::RGB(31,43,61));let _=canvas.fill_rect(Rect::new(160,405,640,18));
    let _=canvas.copy(&texture,None,Rect::new((960-w as i32)/2,320,w,h));canvas.present();
    let (sender,receiver)=mpsc::channel();
    let worker=std::thread::spawn(move|| {
        let progress=sender.clone();
        let result=crate::zip_game::prepare(&zip_path,&cache,|p|{let _=progress.send(Ok(p.percent));});
        let _=sender.send(Err(result));
    });
    let mut events=sdl.event_pump()?;let mut percent=0u8;
    let result='finished: loop {
        loop {match receiver.try_recv() {
            Ok(Ok(p))=>percent=p,
            Ok(Err(result))=>break 'finished result,
            Err(mpsc::TryRecvError::Empty)=>break,
            Err(mpsc::TryRecvError::Disconnected)=>break 'finished Err("ZIP loading worker stopped unexpectedly".into()),
        }}
        for event in events.poll_iter() {if matches!(event,Event::Quit{..}) {eprintln!("ZIP extraction continues until complete");}}
        canvas.set_draw_color(Color::RGB(17,24,37));canvas.clear();
        canvas.set_draw_color(Color::RGB(31,43,61));let _=canvas.fill_rect(Rect::new(160,405,640,18));
        canvas.set_draw_color(Color::RGB(85,174,255));let _=canvas.fill_rect(Rect::new(160,405,640*percent as u32/100,18));
        let _=canvas.copy(&texture,None,Rect::new((960-w as i32)/2,320,w,h));canvas.present();
        std::thread::sleep(Duration::from_millis(30));
    };
    let _=worker.join();
    if let Err(error)=&result {
        eprintln!("leaf-zip loading_failed={error}");
        let (pixels,w,h)=label(&font,&crate::tr!("加载失败，请检查 ZIP 文件", "Loading failed. Check the ZIP file."),30.0);
        let mut failure=creator.create_texture_streaming(PixelFormatEnum::RGBA32,w,h).map_err(|e|e.to_string())?;
        failure.set_blend_mode(sdl2::render::BlendMode::Blend);failure.update(None,&pixels,(w*4) as usize).map_err(|e|e.to_string())?;
        let until=Instant::now()+Duration::from_secs(4);
        while Instant::now()<until {
            if events.poll_iter().any(|e|matches!(e,Event::Quit{..}|Event::KeyDown{..}|Event::ControllerButtonDown{..})){break;}
            canvas.set_draw_color(Color::RGB(17,24,37));canvas.clear();let _=canvas.copy(&failure,None,Rect::new((960-w as i32)/2,330,w,h));canvas.present();
            std::thread::sleep(Duration::from_millis(30));
        }
    }
    result
}
