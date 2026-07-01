# panel.py — controls the Hermes Command Deck (ESP32 desk screen).
# The panel polls these files every ~5s. Just write them; no restart needed.
import json
import os
import shutil
from pathlib import Path

TODOS_FILE  = "/home/phantom/ragnar_todos.json"
CFG_FILE    = "/home/phantom/hermes_cfg.json"
LAYOUT_FILE = "/home/phantom/hermes_layout.json"
GIF_FILE    = "/home/phantom/face.gif"
IMG_FILE    = "/home/phantom/panel_image.png"
VIDEO_FILE  = "/home/phantom/clip.mp4"

def update_todos(todos):
    """todos: list of short strings. Shows on DASHBOARD mode (max 8, ~17 chars)."""
    with open(TODOS_FILE, "w") as f:
        json.dump([str(t) for t in todos][:8], f)

def set_mode(mode, rotation=0, bright=255, accent="00FFFF"):
    """mode: face | video | gif | image | data | layout"""
    with open(CFG_FILE, "w") as f:
        json.dump({"rotation": rotation, "bright": bright,
                   "accent": accent, "mode": mode}, f)

def set_layout(ops):
    """ops: list of draw commands for CUSTOM mode. Example op:
       {"t":"text","x":6,"y":10,"v":"Hello","c":"00FFFF","s":1}
       types: fill,text,rect,line,hline,vline,dot,circle"""
    with open(LAYOUT_FILE, "w") as f:
        json.dump(ops[:24], f)

def set_media(path, mode=None):
    """Install a Discord/uploaded image, GIF, or video for the ESP32 panel.
       The server reloads changed files on the next /frame request.
    """
    src = Path(path).expanduser()
    if not src.exists():
        raise FileNotFoundError(str(src))
    ext = src.suffix.lower()
    if ext == ".gif":
        dest, detected = Path(GIF_FILE), "gif"
    elif ext in (".png", ".jpg", ".jpeg", ".webp"):
        dest, detected = Path(IMG_FILE), "image"
    elif ext in (".mp4", ".mov", ".webm", ".mkv"):
        dest, detected = Path(VIDEO_FILE), "video"
    else:
        raise ValueError(f"Unsupported panel media type: {ext}")
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    shutil.copyfile(src, tmp)
    os.replace(tmp, dest)
    chosen = mode or detected
    set_mode(chosen)
    return {"ok": True, "mode": chosen, "dest": str(dest)}
