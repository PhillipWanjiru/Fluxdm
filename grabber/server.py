# FluxDM — grabber/server.py
# FastAPI server — Java daemon calls this for video detection/grab
from fastapi import FastAPI
from fluxdm_grabber.grabber import Grabber

app     = FastAPI(title="FluxDM Grabber")
grabber = Grabber()

@app.post("/detect")
async def detect(url: str):
    return grabber.detect(url)

@app.post("/grab")
async def grab(url: str, format_id: str = "best", output_dir: str = None):
    return grabber.grab(url, format_id, output_dir)
