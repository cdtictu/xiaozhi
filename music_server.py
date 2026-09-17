#!/usr/bin/env python3
"""May chu nhac cho Xiaozhi.

Bo file nhac (mp3, m4a, flac, wav...) vao ~/xiaozhi/music roi chay:

    python3 ~/xiaozhi/music_server.py          # doi file + phuc vu cong 8080
    python3 ~/xiaozhi/music_server.py --demo   # them 1 am thanh thu, khong can ffmpeg

Script doi file sang Ogg/Opus trong ~/xiaozhi/music/ogg (can ffmpeg:
`sudo apt install ffmpeg`), ghi index.json va phuc vu qua HTTP de ESP32 tai.
File moi bo vao thu muc se tu duoc doi trong vong 10 giay, khong can chay lai.

Bai da doi roi van nam trong danh sach du file goc bi xoa hay doi ten: muon bo
han mot bai thi xoa file .ogg tuong ung trong ogg/.
Thu muc ogg/ do script quan ly: file nhac lo bo vao day se duoc chuyen ra ngoai.

Gui file nhac cho bot Telegram: ESP bao duong dan file qua POST /add, script
tai file tu Telegram (token doc tu sdkconfig), doi sang Opus roi nhan tin bao xong.
"""
import argparse
import functools
import http.server
import json
import os
import re
import shutil
import socket
import subprocess
import threading
import time
import unicodedata
import urllib.request
from pathlib import Path

MUSIC_DIR = Path.home() / "xiaozhi" / "music"
OUT_DIR = MUSIC_DIR / "ogg"
NAMES_FILE = OUT_DIR / "names.json"
SDKCONFIG = Path.home() / "xiaozhi" / "xiaozhi-esp32" / "sdkconfig"
DEMO_SOUND = Path.home() / "xiaozhi" / "xiaozhi-esp32" / "main" / "assets" / "common" / "success.ogg"
AUDIO_EXTS = {".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".oga", ".opus", ".webm", ".wma"}
KEEP_IN_OUT_DIR = {".ogg", ".json", ".part", ".tmp"}
FORMAT_EXTS = {"mp3": ".mp3", "flac": ".flac", "wav": ".wav", "ogg": ".ogg", "aac": ".aac",
               "matroska,webm": ".webm", "mov,mp4,m4a,3gp,3g2,mj2": ".m4a", "asf": ".wma"}
DEMO_FILE = "am-thanh-thu.ogg"
SCAN_INTERVAL_S = 10

DEMO = False
WARNED = set()
LIBRARY_LOCK = threading.Lock()  # Rescan thread and Telegram uploads both rebuild


def slugify(name):
    """Accent-free file name: the ESP matches it and it needs no URL encoding."""
    text = unicodedata.normalize("NFKD", name.replace("đ", "d").replace("Đ", "D"))
    text = text.encode("ascii", "ignore").decode().lower()
    return re.sub(r"[^a-z0-9]+", "-", text).strip("-") or "bai-hat"


def unique_path(folder, stem, ext):
    path = folder / f"{stem}{ext}"
    n = 2
    while path.exists():
        path = folder / f"{stem} ({n}){ext}"
        n += 1
    return path


def load_names():
    """slug -> display name, so a song keeps its name after the source file is gone."""
    try:
        return json.loads(NAMES_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_names(names):
    try:
        NAMES_FILE.write_text(json.dumps(names, ensure_ascii=False, indent=1), encoding="utf-8")
    except OSError as error:
        print(f"  khong ghi duoc names.json ({error.__class__.__name__})")


def probe_extension(path):
    """Extension for a file with no usable one, or None when it is not audio."""
    if shutil.which("ffprobe") is None:
        return None
    try:
        result = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=format_name",
             "-of", "default=nw=1:nk=1", str(path)],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return FORMAT_EXTS.get(result.stdout.strip(), ".mp3")


def convert(src, dst, ffmpeg):
    # Opus 24 kHz mono, 60 ms frames: what the firmware decodes without resampling.
    # Write to a temporary file so an interrupted run never leaves a "finished" song.
    part = dst.with_name(dst.name + ".part")
    subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-i", str(src), "-vn", "-map_metadata", "-1",
         "-c:a", "libopus", "-b:a", "32k", "-ac", "1", "-ar", "24000",
         "-frame_duration", "60", "-application", "audio", "-f", "ogg", str(part)],
        check=True,
    )
    part.replace(dst)


def rescue_misplaced():
    """Songs dropped into ogg/ by mistake: move them back to the music folder."""
    for path in sorted(OUT_DIR.iterdir()):
        if not path.is_file() or path.suffix.lower() in KEEP_IN_OUT_DIR:
            continue
        ext = path.suffix.lower() if path.suffix.lower() in AUDIO_EXTS else probe_extension(path)
        if ext is None:
            continue  # Not an audio file: leave it alone
        target = unique_path(MUSIC_DIR, path.stem, ext)
        path.replace(target)
        print(f"  chuyen {path.name} tu ogg/ ra {target.name} (file nguon phai nam o day)")


def build_library():
    with LIBRARY_LOCK:
        return _build_library()


def _build_library():
    MUSIC_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(exist_ok=True)
    rescue_misplaced()
    ffmpeg = shutil.which("ffmpeg")
    names = load_names()
    songs, used = [], set()

    if DEMO and DEMO_SOUND.exists():
        shutil.copyfile(DEMO_SOUND, OUT_DIR / DEMO_FILE)
        songs.append({"name": "Âm thanh thử", "file": DEMO_FILE})
        used.add(DEMO_FILE)

    sources = sorted(p for p in MUSIC_DIR.iterdir()
                     if p.is_file() and p.suffix.lower() in AUDIO_EXTS)
    slugs = set()
    for src in sources:
        slug = base = slugify(src.stem)
        n = 2
        while slug in slugs:
            slug = f"{base}-{n}"
            n += 1
        slugs.add(slug)
        dst = OUT_DIR / f"{slug}.ogg"
        if not dst.exists() or dst.stat().st_mtime < src.stat().st_mtime:
            if ffmpeg is None:
                if src.name not in WARNED:
                    print(f"  BO QUA {src.name}: chua cai ffmpeg (sudo apt install ffmpeg)")
                    WARNED.add(src.name)
                continue
            print(f"  doi {src.name} -> {dst.name} (file dai co the mat vai phut)", flush=True)
            started = time.time()
            try:
                convert(src, dst, ffmpeg)
            except subprocess.CalledProcessError:
                print(f"  LOI khi doi {src.name}")
                continue
            print(f"  xong {dst.name}: {dst.stat().st_size // 1024} KB, "
                  f"{time.time() - started:.0f} giay", flush=True)
        # Linux file names may keep accents decomposed (NFD); speech and Telegram
        # text arrive composed (NFC), so list NFC names for the ESP to match
        display = unicodedata.normalize("NFC", src.stem)
        names[dst.stem] = display
        songs.append({"name": display, "file": dst.name})
        used.add(dst.name)

    # Keep songs whose source file was moved, renamed or deleted: the .ogg still plays
    for path in sorted(OUT_DIR.glob("*.ogg")):
        if path.name in used or (path.name == DEMO_FILE and not DEMO):
            continue
        songs.append({"name": names.get(path.stem, path.stem.replace("-", " ")),
                      "file": path.name})

    songs.sort(key=lambda song: song["name"].casefold())
    save_names(names)
    # Write then rename, so the ESP never reads a half-written index
    tmp = OUT_DIR / "index.json.tmp"
    tmp.write_text(json.dumps({"songs": songs}, ensure_ascii=False, indent=1), encoding="utf-8")
    tmp.replace(OUT_DIR / "index.json")
    return songs


def telegram_config():
    """Bot token and owner chat ID, from the firmware sdkconfig or the environment."""
    values = {}
    try:
        for line in SDKCONFIG.read_text(encoding="utf-8").splitlines():
            match = re.match(r'CONFIG_(TELEGRAM_BOT_TOKEN|TELEGRAM_CHAT_ID)="(.*)"$', line)
            if match:
                values[match.group(1)] = match.group(2)
    except OSError:
        pass
    token = os.environ.get("XIAOZHI_BOT_TOKEN") or values.get("TELEGRAM_BOT_TOKEN", "")
    chat_id = os.environ.get("XIAOZHI_CHAT_ID") or values.get("TELEGRAM_CHAT_ID", "")
    return token, chat_id


def notify(text):
    token, chat_id = telegram_config()
    if not token or not chat_id:
        return
    request = urllib.request.Request(
        f"https://api.telegram.org/bot{token}/sendMessage",
        data=json.dumps({"chat_id": chat_id, "text": text}).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        urllib.request.urlopen(request, timeout=15).close()
    except OSError as error:
        # Never print the URL: it contains the bot token
        print(f"  khong gui duoc tin Telegram ({error.__class__.__name__})")


def add_from_telegram(file_path, name):
    token, _ = telegram_config()
    if not token:
        print("  khong thay token bot trong sdkconfig")
        return
    ext = Path(file_path).suffix.lower()
    if ext == ".oga":
        ext = ".ogg"
    if ext not in AUDIO_EXTS:
        ext = ".mp3"
    safe = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "", name).strip()[:80] or "Bai hat"
    dst = unique_path(MUSIC_DIR, safe, ext)

    # Download under a name the scanner skips, so a half-written file is never converted
    part = MUSIC_DIR / f".{dst.name}.part"
    print(f"  tai tu Telegram: {dst.name}")
    try:
        url = f"https://api.telegram.org/file/bot{token}/{file_path}"
        with urllib.request.urlopen(url, timeout=120) as response, open(part, "wb") as out:
            shutil.copyfileobj(response, out)
        part.replace(dst)
    except OSError as error:
        part.unlink(missing_ok=True)
        notify(f"❌ Tải “{name}” từ Telegram thất bại ({error.__class__.__name__}).")
        return

    songs = build_library()
    for number, song in enumerate(songs, 1):
        if song["name"] == unicodedata.normalize("NFC", dst.stem):
            notify(f"✅ Đã thêm “{dst.stem}” — bài số {number}.\nPhát: /nhac {number}")
            return
    notify(f"⚠️ Đã tải “{dst.stem}” về máy tính nhưng chưa đổi được sang Opus. "
           "Máy tính đã cài ffmpeg chưa? (sudo apt install ffmpeg)")


class MusicHandler(http.server.SimpleHTTPRequestHandler):
    """GET serves index.json and the .ogg files; POST /add queues a Telegram upload."""

    def copyfile(self, source, outputfile):
        try:
            super().copyfile(source, outputfile)
        except (ConnectionResetError, BrokenPipeError):
            pass  # The ESP stopped the song and closed the stream

    def do_POST(self):
        if self.path != "/add":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(min(length, 8192)) or b"{}")
            file_path = str(data["file_path"])
            name = str(data.get("name") or "Bài hát")
        except (ValueError, KeyError, TypeError):
            self.send_error(400)
            return
        # Telegram file paths look like "music/file_12.mp3"
        if not re.fullmatch(r"[A-Za-z0-9_./-]{1,200}", file_path) or ".." in file_path:
            self.send_error(400)
            return
        # Downloading and converting take longer than the ESP waits: answer now
        threading.Thread(target=add_from_telegram, args=(file_path, name), daemon=True).start()
        self.send_response(202)
        self.send_header("Content-Length", "0")
        self.end_headers()


def lan_ip():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def main():
    global DEMO
    parser = argparse.ArgumentParser(description="May chu nhac cho Xiaozhi")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--demo", action="store_true", help="them 1 am thanh thu, khong can ffmpeg")
    args = parser.parse_args()
    DEMO = args.demo

    token, chat_id = telegram_config()
    print("Nhan nhac gui qua Telegram: " +
          ("co" if token and chat_id else "KHONG (thieu token/chat ID trong sdkconfig)"))

    # Convert in the background so the server answers the ESP right away
    def rescan():
        reported = False
        while True:
            try:
                songs = build_library()
                if not reported:
                    print(f"{len(songs)} bai trong {OUT_DIR}", flush=True)
                    for number, song in enumerate(songs, 1):
                        print(f"  {number}. {song['name']}", flush=True)
                    reported = True
            except Exception as error:  # One bad file must not stop future scans
                print(f"  loi khi quet thu muc ({error.__class__.__name__}: {error})", flush=True)
            time.sleep(SCAN_INTERVAL_S)

    threading.Thread(target=rescan, daemon=True).start()

    handler = functools.partial(MusicHandler, directory=str(OUT_DIR))
    with http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler) as httpd:
        print(f"\nMay chu nhac: http://{lan_ip()}:{args.port}/   (Ctrl+C de dung)", flush=True)
        print(f"Bo file nhac vao {MUSIC_DIR} la tu co trong danh sach.", flush=True)
        httpd.serve_forever()


if __name__ == "__main__":
    main()
