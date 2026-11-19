"""Controller-operated game picker for the AisOS RGBox application menu."""
import ctypes
import os
from contextlib import ExitStack
from pathlib import Path
import subprocess
import sys

APP = Path(__file__).resolve().parent
sys.path.insert(0, str(APP / "pysdl2.whl"))
import sdl2 as sdl
from sdl2 import sdlttf as ttf

WIDTH, HEIGHT = 1024, 768
PAGE = 8
TEXT = (242, 246, 248)
MUTED = (175, 194, 205)
KEYS = {
    sdl.SDLK_UP: "up", sdl.SDLK_DOWN: "down",
    sdl.SDLK_RETURN: "open", sdl.SDLK_ESCAPE: "back",
}
BUTTONS = {
    sdl.SDL_CONTROLLER_BUTTON_DPAD_UP: "up",
    sdl.SDL_CONTROLLER_BUTTON_DPAD_DOWN: "down",
    sdl.SDL_CONTROLLER_BUTTON_LEFTSHOULDER: "page_up",
    sdl.SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: "page_down",
    sdl.SDL_CONTROLLER_BUTTON_A: "open",
    sdl.SDL_CONTROLLER_BUTTON_START: "open",
    sdl.SDL_CONTROLLER_BUTTON_B: "back",
}


def stick_action(event, directions):
    if event.axis not in (sdl.SDL_CONTROLLER_AXIS_LEFTY, sdl.SDL_CONTROLLER_AXIS_RIGHTY):
        return None
    if abs(event.value) <= 8000:
        directions.pop(event.axis, None)
    elif abs(event.value) >= 16000:
        action = "down" if event.value > 0 else "up"
        if directions.get(event.axis) != action:
            directions[event.axis] = action
            return action
    return None


def error():
    return RuntimeError(sdl.SDL_GetError().decode("utf-8", "replace"))


def entries(folder):
    items = []
    for path in folder.iterdir():
        if path.name.startswith(".") or path.is_symlink():
            continue
        if path.is_dir() or (path.suffix.lower() in {".swf", ".zip"} and path.is_file()):
            items.append(path)
    return sorted(items, key=lambda p: (not p.is_dir(), p.name.casefold(), p.name))


def label(renderer, font, value, x, y, color, width):
    surface = ttf.TTF_RenderUTF8_Blended(font, value.encode("utf-8", "replace"),
                                         sdl.SDL_Color(*color, 255))
    if not surface:
        raise error()
    try:
        texture = sdl.SDL_CreateTextureFromSurface(renderer, surface)
        if not texture:
            raise error()
        try:
            w, h = min(width, surface.contents.w), surface.contents.h
            src, dst = sdl.SDL_Rect(0, 0, w, h), sdl.SDL_Rect(x, y, w, h)
            sdl.SDL_RenderCopy(renderer, texture, src, dst)
        finally:
            sdl.SDL_DestroyTexture(texture)
    finally:
        sdl.SDL_FreeSurface(surface)


def draw(renderer, title_font, font, folder, root, items, index, message):
    sdl.SDL_SetRenderDrawColor(renderer, 19, 31, 40, 255)
    sdl.SDL_RenderClear(renderer)
    label(renderer, title_font, "Ruffle Flash", 48, 28, TEXT, 700)
    page = index // PAGE
    label(renderer, font, f"{page+1} / {max(1, (len(items)+PAGE-1)//PAGE)}",
          850, 44, MUTED, 134)
    label(renderer, font, str(folder.relative_to(root.parent)), 48, 101, MUTED, 920)
    for row, path in enumerate(items[page * PAGE:(page + 1) * PAGE]):
        y = 160 + row * 58
        if row + page * PAGE == index:
            sdl.SDL_SetRenderDrawColor(renderer, 39, 96, 103, 255)
            rect = sdl.SDL_Rect(34, y, 956, 52)
            sdl.SDL_RenderFillRect(renderer, rect)
        label(renderer, font, path.name + ("/" if path.is_dir() else ""), 52, y+8, TEXT, 916)
    if not items:
        label(renderer, font, "此目录没有游戏。请将 SWF 或 ZIP 放入 flash 文件夹。",
              48, 190, MUTED, 928)
    if message:
        label(renderer, font, message, 48, 636, (255, 201, 128), 928)
    label(renderer, font, "十字键 / 摇杆  选择     L1 / R1  翻页     A  打开     B  返回", 48, 684, MUTED, 928)
    label(renderer, font, "游戏中：按下右摇杆 (R3) 打开暂停菜单", 48, 724, MUTED, 928)
    sdl.SDL_RenderPresent(renderer)


def choose(root, folder, index, message):
    with ExitStack() as cleanup:
        if sdl.SDL_Init(sdl.SDL_INIT_VIDEO | sdl.SDL_INIT_GAMECONTROLLER) < 0:
            raise error()
        cleanup.callback(sdl.SDL_Quit)
        if ttf.TTF_Init() < 0:
            raise error()
        cleanup.callback(ttf.TTF_Quit)
        window = sdl.SDL_CreateWindow(b"Ruffle Flash", 0, 0, WIDTH, HEIGHT,
                                      sdl.SDL_WINDOW_FULLSCREEN_DESKTOP)
        if not window:
            raise error()
        cleanup.callback(sdl.SDL_DestroyWindow, window)
        renderer = sdl.SDL_CreateRenderer(window, -1, 0)
        if not renderer:
            raise error()
        cleanup.callback(sdl.SDL_DestroyRenderer, renderer)
        sdl.SDL_RenderSetLogicalSize(renderer, WIDTH, HEIGHT)
        sdl.SDL_ShowCursor(sdl.SDL_DISABLE)
        fonts = []
        for size in (48, 26):
            font = ttf.TTF_OpenFont(os.fsencode(APP / "MiSans-Normal.ttf"), size)
            if not font:
                raise error()
            cleanup.callback(ttf.TTF_CloseFont, font)
            fonts.append(font)

        pads = {}
        stick_directions = {}
        def close_pads():
            for pad in pads.values():
                sdl.SDL_GameControllerClose(pad)
        cleanup.callback(close_pads)
        items = entries(folder)
        event = sdl.SDL_Event()
        dirty = True
        print("picker ready", folder, len(items))
        while True:
            if dirty:
                index = max(0, min(index, len(items) - 1))
                draw(renderer, *fonts, folder, root, items, index, message)
                dirty = False
            if not sdl.SDL_WaitEvent(ctypes.byref(event)):
                raise error()
            action = None
            if event.type == sdl.SDL_CONTROLLERDEVICEADDED:
                pad = sdl.SDL_GameControllerOpen(event.cdevice.which)
                if pad:
                    instance = sdl.SDL_JoystickInstanceID(sdl.SDL_GameControllerGetJoystick(pad))
                    if instance in pads:
                        sdl.SDL_GameControllerClose(pad)
                    else:
                        pads[instance] = pad
                        stick_directions[instance] = {}
            elif event.type == sdl.SDL_CONTROLLERDEVICEREMOVED:
                pad = pads.pop(event.cdevice.which, None)
                stick_directions.pop(event.cdevice.which, None)
                if pad:
                    sdl.SDL_GameControllerClose(pad)
            elif event.type == sdl.SDL_WINDOWEVENT:
                if event.window.event == sdl.SDL_WINDOWEVENT_CLOSE:
                    return None
                dirty = True
            elif event.type == sdl.SDL_QUIT:
                return None
            elif event.type == sdl.SDL_KEYDOWN and not event.key.repeat:
                action = KEYS.get(event.key.keysym.sym)
            elif event.type == sdl.SDL_CONTROLLERBUTTONDOWN:
                action = BUTTONS.get(event.cbutton.button)
            elif event.type == sdl.SDL_CONTROLLERAXISMOTION:
                directions = stick_directions.get(event.caxis.which)
                if directions is not None:
                    action = stick_action(event.caxis, directions)
            if not action:
                continue
            dirty, message = True, ""
            if action == "up":
                index -= 1
            elif action == "down":
                index += 1
            elif action == "page_up":
                index -= PAGE
            elif action == "page_down":
                index += PAGE
            elif action == "back":
                if folder == root:
                    return None
                child, folder = folder, folder.parent
                items = entries(folder)
                index = items.index(child) if child in items else 0
            elif action == "open" and items:
                selected = items[index]
                if selected.is_symlink() or not selected.exists():
                    items = entries(folder)
                elif selected.is_dir():
                    try:
                        items = entries(selected)
                    except OSError as exc:
                        print(exc, file=sys.stderr)
                        message = "无法读取所选文件夹"
                    else:
                        folder, index = selected, 0
                else:
                    return selected, index


def main():
    root = Path(os.environ["RUFFLE_FLASH_DIR"]).resolve(strict=True)
    folder, index, message = root, 0, ""
    while choice := choose(root, folder, index, message):
        game, index = choice
        folder = game.parent
        print("picker launch", game)
        try:
            result = subprocess.run([APP / "ruffle", game, "--fullscreen", "--renderer", "opengl-es"])
            message = "" if result.returncode == 0 else f"游戏退出码 {result.returncode}，详见 session.log"
        except OSError as exc:
            print(exc, file=sys.stderr)
            message = "无法启动播放器，详见 session.log"
    print("picker exit")


if __name__ == "__main__":
    main()
