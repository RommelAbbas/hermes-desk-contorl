#!/usr/bin/env python3
# =====================================================================
#  HERMES COMMAND DECK - (OPTIMIZED)
#  ESP32 untouched. Faster: cached status, cached cfg, pre-rendered
#  face frames, numpy packing, no per-request network blocking.
# =====================================================================
import os, time, json, socket, subprocess, math, threading, logging
from flask import Flask, jsonify, Response, request
from PIL import Image, ImageDraw, ImageSequence
import numpy as np

app = Flask(__name__)
logging.getLogger('werkzeug').setLevel(logging.ERROR)   # silence per-request log spam
BOOT = time.time()
HOME = os.path.expanduser("~")
CFG_FILE    = os.path.join(HOME, "hermes_cfg.json")
TODOS_FILE  = os.path.join(HOME, "ragnar_todos.json")
LAYOUT_FILE = os.path.join(HOME, "hermes_layout.json")
CLIP        = os.path.join(HOME, "clip.mp4")
GIF         = os.path.join(HOME, "face.gif")
IMG         = os.path.join(HOME, "panel_image.png")

VPS_IP="87.99.132.132"; VPS_PORT=22; RAGNAR_PROC="hermes"
#COUNTDOWNS={"RIT FORM":"2026-06-30 23:59","VISA INTV":"2026-08-31 09:00"}

DEFAULT_MENU = [
    {"label":"FACE","action":"mode:face"},
    {"label":"VIDEO","action":"mode:video"},
    {"label":"GIF","action":"mode:gif"},
    {"label":"IMAGE","action":"mode:image"},
    {"label":"DASHBOARD","action":"mode:data"},
    {"label":"CUSTOM","action":"mode:layout"},
]

# ---------- fast RGB565 packing (numpy) ----------
def img_to_565(img):
    img = img.convert("RGB")
    if img.size != (128,128):
        r=max(128/img.width,128/img.height)
        img=img.resize((round(img.width*r),round(img.height*r)))
        l=(img.width-128)//2; t=(img.height-128)//2
        img=img.crop((l,t,l+128,t+128))
    a=np.asarray(img,dtype=np.uint16)
    v=((a[:,:,0]&0xF8)<<8)|((a[:,:,1]&0xFC)<<3)|(a[:,:,2]>>3)
    return v.astype(">u2").tobytes()

# ---------- frame sources ----------
VIDEO_FRAMES=[]; GIF_FRAMES=[]; IMG_FRAME=None; FACE_FRAMES={}; FACE_N=30
_MEDIA_MTIME={"video":None,"gif":None,"image":None}
_MEDIA_LOCK=threading.Lock()

def _mtime(path):
    try: return os.path.getmtime(path)
    except OSError: return None

def load_video_frames(force=False):
    global VIDEO_FRAMES
    m=_mtime(CLIP)
    if m is None:
        VIDEO_FRAMES=[]; _MEDIA_MTIME["video"]=None; return
    if not force and m==_MEDIA_MTIME.get("video"): return
    try:
        import av
        frames=[img_to_565(f.to_image()) for f in av.open(CLIP).decode(video=0)]
        VIDEO_FRAMES=frames; _MEDIA_MTIME["video"]=m
        print("video frames:",len(VIDEO_FRAMES))
    except Exception as e: print("video load fail:",e)

def load_gif_frames(force=False):
    global GIF_FRAMES
    m=_mtime(GIF)
    if m is None:
        GIF_FRAMES=[]; _MEDIA_MTIME["gif"]=None; return
    if not force and m==_MEDIA_MTIME.get("gif"): return
    try:
        GIF_FRAMES=[img_to_565(f.copy()) for f in ImageSequence.Iterator(Image.open(GIF))]
        _MEDIA_MTIME["gif"]=m
        print("gif frames:",len(GIF_FRAMES))
    except Exception as e: print("gif load fail:",e)

def load_image_frame(force=False):
    global IMG_FRAME
    m=_mtime(IMG)
    if m is None:
        IMG_FRAME=None; _MEDIA_MTIME["image"]=None; return
    if not force and m==_MEDIA_MTIME.get("image"): return
    try:
        IMG_FRAME=img_to_565(Image.open(IMG)); _MEDIA_MTIME["image"]=m
        print("image loaded")
    except Exception as e: print("image load fail:",e)

def refresh_media(mode=None):
    # Called by /frame so a replaced GIF/image/video from Discord becomes live without restarting.
    with _MEDIA_LOCK:
        if mode in (None,"video"): load_video_frames()
        if mode in (None,"gif"): load_gif_frames()
        if mode in (None,"image"): load_image_frame()

def preload():
    refresh_media()
    build_faces()

def render_face_frame(expr,k):
    img=Image.new("RGB",(128,128),(0,0,0)); d=ImageDraw.Draw(img)
    cfg=load_cfg(); acc=cfg.get("accent","00FFFF")
    col=tuple(int(acc[i:i+2],16) for i in (0,2,4))
    ph=k/FACE_N*2*math.pi
    look=int(8*math.sin(ph))
    blink=(k%FACE_N) in (0,1)              # quick blink at cycle start
    eh=4 if blink else 26
    ex1,ex2=36+look,76+look; ey=44
    d.rounded_rectangle([ex1,ey,ex1+18,ey+eh],radius=5,fill=col)
    d.rounded_rectangle([ex2,ey,ex2+18,ey+eh],radius=5,fill=col)
    if expr=="alert":
        d.line([48,92,80,92],fill=(255,60,60),width=4)
    elif expr=="happy":
        d.arc([44,78,84,104],200,340,fill=col,width=4)
    else:
        w=22+int(6*math.sin(ph*2))
        d.line([64-w//2,92,64+w//2,92],fill=(0,255,150),width=4)
    return img_to_565(img)

def build_faces():
    global FACE_FRAMES
    FACE_FRAMES={e:[render_face_frame(e,k) for k in range(FACE_N)] for e in ("normal","happy","alert")}
    print("face frames built")

# ---------- cached system status (refreshed in background) ----------
_status={"ragnar":False,"vps":False,"temp":0.0,"load":0.0,"uptime":"--"}

def tcp_alive(h,p,t=2):
    try:
        with socket.create_connection((h,p),timeout=t): return True
    except OSError: return False
def ragnar_status():
    try: return bool(subprocess.check_output(["pgrep","-f",RAGNAR_PROC]).strip())
    except Exception: return False
def uptime_str():
    s=int(time.time()-BOOT)
    try:
        with open("/proc/uptime") as f: s=int(float(f.read().split()[0]))
    except Exception: pass
    d,s=divmod(s,86400);h,s=divmod(s,3600);m,_=divmod(s,60)
    return f"{d}d {h}h" if d else (f"{h}h {m}m" if h else f"{m}m")
def temp_load():
    load=os.getloadavg()[0] if hasattr(os,"getloadavg") else 0.0
    temp=0.0
    try:
        import psutil
        for _,e in psutil.sensors_temperatures().items():
            if e: temp=e[0].current; break
    except Exception: pass
    return round(temp,1),round(load,2)

def status_loop():
    while True:
        try:
            _status["ragnar"]=ragnar_status()
            _status["vps"]=tcp_alive(VPS_IP,VPS_PORT)
            t,l=temp_load(); _status["temp"]=t; _status["load"]=l
            _status["uptime"]=uptime_str()
        except Exception as e: print("status err:",e)
        time.sleep(8)

def current_expr():
    if not _status["vps"] or not _status["ragnar"]: return "alert"
    return "happy"

# ---------- cached cfg ----------
_cfg={"v":None,"mtime":0}
def load_cfg():
    base={"rotation":0,"bright":255,"accent":"00FFFF","mode":"face"}
    try:
        m=os.path.getmtime(CFG_FILE)
        if _cfg["v"] is None or m!=_cfg["mtime"]:
            with open(CFG_FILE) as f: c=json.load(f)
            base.update(c); _cfg["v"]=base; _cfg["mtime"]=m
        return _cfg["v"]
    except Exception:
        return base
def save_cfg(c):
    with open(CFG_FILE,"w") as f: json.dump(c,f)
    _cfg["v"]=None   # force reload

def get_todos():
    try:
        with open(TODOS_FILE) as f:
            data=json.load(f)
            if isinstance(data,list): return [str(x) for x in data][:8]
    except Exception: pass
    return []
def load_layout():
    try:
        with open(LAYOUT_FILE) as f:
            data=json.load(f)
            if isinstance(data,list): return data[:24]
    except Exception: pass
    return []
def countdowns():
    out,now=[],time.time()
    for n,w in COUNTDOWNS.items():
        try:
            t=time.mktime(time.strptime(w,"%Y-%m-%d %H:%M"));s=int(t-now)
            if s<0: out.append({"name":n,"left":"passed"})
            else:
                d,r=divmod(s,86400);h,_=divmod(r,3600);out.append({"name":n,"left":f"{d}d {h}h"})
        except Exception: out.append({"name":n,"left":"?"})
    return out

# ---------- routes ----------
@app.route("/panel")
def panel():
    cfg=load_cfg()
    return jsonify({
        "mode":cfg.get("mode","face"),
        "onyx":True,"ragnar":_status["ragnar"],"vps":_status["vps"],
        "uptime":_status["uptime"],"temp":_status["temp"],"load":_status["load"],
        "todos":get_todos(),"countdowns":countdowns(),
        "menu":DEFAULT_MENU,"layout":load_layout(),
        "cfg":{"rotation":cfg.get("rotation",0),"bright":cfg.get("bright",255),"accent":cfg.get("accent","00FFFF")}
    })

@app.route("/set")
def set_mode():
    cfg=load_cfg().copy()
    for k in ("mode","accent"):
        if k in request.args: cfg[k]=request.args.get(k)
    for k in ("rotation","bright"):
        if k in request.args:
            try: cfg[k]=int(request.args.get(k))
            except: pass
    save_cfg(cfg)
    return jsonify({"ok":True,"cfg":cfg})

@app.route("/frame")
def frame():
    mode=load_cfg().get("mode","face")
    if mode in ("video","gif","image"):
        refresh_media(mode)
    if mode=="video" and VIDEO_FRAMES:
        return Response(VIDEO_FRAMES[int(time.time()*12)%len(VIDEO_FRAMES)],mimetype="application/octet-stream")
    if mode=="gif" and GIF_FRAMES:
        return Response(GIF_FRAMES[int(time.time()*10)%len(GIF_FRAMES)],mimetype="application/octet-stream")
    if mode=="image" and IMG_FRAME:
        return Response(IMG_FRAME,mimetype="application/octet-stream")
    fr=FACE_FRAMES.get(current_expr()) or FACE_FRAMES.get("normal") or []
    if fr:
        return Response(fr[int(time.time()*10)%len(fr)],mimetype="application/octet-stream")
    return Response(b"\x00"*32768,mimetype="application/octet-stream")

if __name__=="__main__":
    if not os.path.exists(CFG_FILE):
        save_cfg({"rotation":0,"bright":255,"accent":"00FFFF","mode":"face"})
    preload()
    threading.Thread(target=status_loop,daemon=True).start()
    app.run(host="0.0.0.0",port=8090,threaded=True)
