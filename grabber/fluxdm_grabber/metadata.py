# FluxDM — metadata.py
import yt_dlp

def extract_metadata(url: str) -> dict:
    opts = {"quiet": True, "skip_download": True}
    with yt_dlp.YoutubeDL(opts) as ydl:
        try:
            info = ydl.extract_info(url, download=False)
            formats = [
                {"id": f["format_id"], "ext": f.get("ext"),
                 "height": f.get("height"), "abr": f.get("abr")}
                for f in info.get("formats", [])
            ]
            return {
                "title":     info.get("title"),
                "thumbnail": info.get("thumbnail"),
                "duration":  info.get("duration"),
                "formats":   formats,
            }
        except Exception as e:
            return {"error": str(e)}
