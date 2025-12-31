# FluxDM — grabber/fluxdm_grabber/grabber.py
import yt_dlp
from .site_detector import detect_site
from .metadata import extract_metadata

class Grabber:
    def detect(self, url: str) -> dict:
        site = detect_site(url)
        meta = extract_metadata(url)
        return {"site": site, **meta}

    def grab(self, url: str, format_id: str = "best", output_dir: str = None) -> dict:
        from config import OUTPUT_DIR
        opts = {
            "format": format_id,
            "outtmpl": f"{output_dir or OUTPUT_DIR}/%(title)s.%(ext)s",
            "quiet": True,
        }
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(url, download=True)
        return {"status": "ok", "title": info.get("title"), "url": url}
