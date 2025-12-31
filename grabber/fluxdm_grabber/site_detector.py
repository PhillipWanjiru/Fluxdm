# FluxDM — site_detector.py
import re

PATTERNS = {
    "youtube":   r"(youtube\.com|youtu\.be)",
    "tiktok":    r"tiktok\.com",
    "twitter":   r"(twitter\.com|x\.com)",
    "instagram": r"instagram\.com",
    "facebook":  r"facebook\.com",
}

def detect_site(url: str) -> str:
    for name, pattern in PATTERNS.items():
        if re.search(pattern, url, re.I):
            return name
    return "generic"
