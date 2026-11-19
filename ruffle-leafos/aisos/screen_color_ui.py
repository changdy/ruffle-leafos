"""RGBox gamepad interface for built-in display white balance."""
from contextlib import ExitStack
import os
from pathlib import Path
import sys

from screen_color import MAX_LEVEL, PREVIEW, exclusive, panel, save_level, saved_level

APP = Path(__file__).resolve().parent
sys.path.insert(0, str(APP / "pysdl2.whl"))
import sdl2 as sdl
from sdl2 import sdlttf as ttf


WIDTH, HEIGHT = 1024, 768
BUTTONS = {
    sdl.SDL_CONTROLLER_BUTTON_DPAD_LEFT: "left",
    sdl.SDL_CONTROLLER_BUTTON_DPAD_RIGHT: "right",
    sdl.SDL_CONTROLLER_BUTTON_A: "save",
    sdl.SDL_CONTROLLER_BUTTON_B: "back",
    sdl.SDL_CONTROLLER_BUTTON_X: "reset",
}
KEYS = {
    sdl.SDLK_LEFT: "left", sdl.SDLK_RIGHT: "right",
    sdl.SDLK_RETURN: "save", sdl.SDLK_ESCAPE: "back", sdl.SDLK_x: "reset",
}


def sdl_error():
    return RuntimeError(sdl.SDL_GetError().decode("utf-8", "replace"))


def text(renderer, font, value, x, y, color=(241, 244, 247)):
    surface = ttf.TTF_RenderUTF8_Blended(font, value.encode(), sdl.SDL_Color(*color, 255))
    if not surface:
        raise sdl_error()
    try:
        texture = sdl.SDL_CreateTextureFromSurface(renderer, surface)
        if not texture:
            raise sdl_error()
        try:
            rect = sdl.SDL_Rect(x, y, surface.contents.w, surface.contents.h)
            sdl.SDL_RenderCopy(renderer, texture, None, rect)
        finally:
            sdl.SDL_DestroyTexture(texture)
    finally:
        sdl.SDL_FreeSurface(surface)


def box(renderer, color, x, y, width, height):
    sdl.SDL_SetRenderDrawColor(renderer, *color, 255)
    sdl.SDL_RenderFillRect(renderer, sdl.SDL_Rect(x, y, width, height))


def draw(renderer, fonts, level, message):
    title, normal = fonts
    sdl.SDL_SetRenderDrawColor(renderer, 27, 29, 33, 255)
    sdl.SDL_RenderClear(renderer)
    text(renderer, title, "屏幕调色", 56, 42)
    text(renderer, normal, f"暖色强度   {level:02d} / {MAX_LEVEL}", 56, 138)
    box(renderer, (76, 81, 88), 60, 205, 904, 8)
    if level:
        box(renderer, (234, 171, 102), 60, 205, 904 * level // MAX_LEVEL, 8)
    box(renderer, (245, 205, 154), 53 + 904 * level // MAX_LEVEL, 195, 16, 28)
    text(renderer, normal, "默认", 56, 230)
    text(renderer, normal, "更暖", 914, 230)
    text(renderer, normal, "用白色和灰阶判断偏色", 56, 303)
    for index, gray in enumerate((0, 36, 73, 109, 146, 182, 219, 255)):
        box(renderer, (gray, gray, gray), 56 + index * 114, 358, 114, 126)
    for index, color in enumerate(((230, 60, 60), (244, 204, 57), (70, 193, 102),
                                   (79, 209, 220), (66, 120, 231), (202, 87, 196))):
        box(renderer, color, 56 + index * 152, 492, 152, 56)
    if message:
        text(renderer, normal, message, 56, 579, (255, 194, 128))
    text(renderer, normal, "← →  调节     X  恢复默认     A  保存并退出     B  取消", 56, 671)
    sdl.SDL_RenderPresent(renderer)


def run(display, initial, unavailable):
    with ExitStack() as cleanup:
        if sdl.SDL_Init(sdl.SDL_INIT_VIDEO | sdl.SDL_INIT_GAMECONTROLLER) < 0:
            raise sdl_error()
        cleanup.callback(sdl.SDL_Quit)
        if ttf.TTF_Init() < 0:
            raise sdl_error()
        cleanup.callback(ttf.TTF_Quit)
        window = sdl.SDL_CreateWindow(b"Screen Color", 0, 0, WIDTH, HEIGHT,
                                      sdl.SDL_WINDOW_FULLSCREEN_DESKTOP)
        if not window:
            raise sdl_error()
        cleanup.callback(sdl.SDL_DestroyWindow, window)
        renderer = sdl.SDL_CreateRenderer(window, -1, 0)
        if not renderer:
            raise sdl_error()
        cleanup.callback(sdl.SDL_DestroyRenderer, renderer)
        sdl.SDL_RenderSetLogicalSize(renderer, WIDTH, HEIGHT)
        sdl.SDL_ShowCursor(sdl.SDL_DISABLE)
        fonts = []
        for size in (52, 26):
            font = ttf.TTF_OpenFont(os.fsencode(APP / "panel-font.ttf"), size)
            if not font:
                raise sdl_error()
            cleanup.callback(ttf.TTF_CloseFont, font)
            fonts.append(font)

        pads = {}

        def open_pad(index):
            if sdl.SDL_IsGameController(index):
                instance = sdl.SDL_JoystickGetDeviceInstanceID(index)
                if instance in pads:
                    return
                pad = sdl.SDL_GameControllerOpen(index)
                if pad:
                    pads[instance] = pad

        def close_pads():
            for pad in pads.values():
                sdl.SDL_GameControllerClose(pad)

        cleanup.callback(close_pads)
        for index in range(sdl.SDL_NumJoysticks()):
            open_pad(index)
        event = sdl.SDL_Event()
        level = initial
        message = unavailable
        draw(renderer, fonts, level, message)

        while sdl.SDL_WaitEvent(event):
            if event.type == sdl.SDL_QUIT:
                return False
            if event.type == sdl.SDL_CONTROLLERDEVICEADDED:
                open_pad(event.cdevice.which)
                continue
            if event.type == sdl.SDL_CONTROLLERDEVICEREMOVED:
                pad = pads.pop(event.cdevice.which, None)
                if pad:
                    sdl.SDL_GameControllerClose(pad)
                continue
            if event.type == sdl.SDL_KEYDOWN:
                action = KEYS.get(event.key.keysym.sym)
            elif event.type == sdl.SDL_CONTROLLERBUTTONDOWN and event.cbutton.which in pads:
                action = BUTTONS.get(event.cbutton.button)
            else:
                continue
            if action == "back":
                return False
            if not action or display is None:
                continue
            try:
                if action == "save":
                    display.apply(level)
                    save_level(level)
                    return True
                next_level = {"left": max(0, level - 1), "right": min(MAX_LEVEL, level + 1),
                              "reset": 0}[action]
                if next_level != level:
                    PREVIEW.touch()
                    display.apply(next_level)
                    level = next_level
                message = ""
            except (OSError, ValueError) as error:
                print(error, file=sys.stderr, flush=True)
                message = "设置失败，请重试。按 B 返回。"
            draw(renderer, fonts, level, message)
        raise sdl_error()


def main():
    with exclusive(nonblocking=True) as acquired, ExitStack() as cleanup:
        if not acquired:
            return
        try:
            initial = saved_level() or 0
            display = cleanup.enter_context(panel())
        except (OSError, ValueError) as error:
            print(error, file=sys.stderr, flush=True)
            run(None, 0, "无法读取内屏调色设置。按 B 返回。")
            return
        committed = False
        try:
            # Recover an interrupted preview before opening a new session.
            if PREVIEW.exists():
                display.apply(initial)
                PREVIEW.unlink()
            committed = run(display, initial, "")
        finally:
            if PREVIEW.exists():
                if not committed:
                    display.apply(initial)
                PREVIEW.unlink()


if __name__ == "__main__":
    main()
