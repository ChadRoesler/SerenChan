#!/usr/bin/env python3
"""The Seren voice link's first server: stands where Xiaozhi's cloud stood.

The device checks in at POST /ota/, is told to use our websocket, and speaks Xiaozhi's protocol to
/voice (see firmware/docs/voice-link.md). What it hears goes to Lodestar's hear route and lands in
state/heard.jsonl. What it says comes from Lodestar's speak route, in my voice, by POST /say.

There is no mind in here yet: whoever answers reads heard.jsonl and POSTs /say.

    .venv\\Scripts\\python seren_voice.py --advertise 192.168.0.110
    curl -X POST -H "Authorization: Bearer <state/token>" -d "{\\"text\\":\\"hi\\"}" http://localhost:7480/say
"""
import argparse
import asyncio
import io
import json
import logging
import re
import secrets
import time
import wave
from pathlib import Path

import av
import numpy as np
from aiohttp import ClientSession, ClientTimeout, FormData, WSMsgType, web

log = logging.getLogger("seren-voice")
HERE = Path(__file__).resolve().parent
STATE = HERE / "state"
HEARD_DIR = STATE / "heard"
HEARD_LOG = STATE / "heard.jsonl"
TOUCH_LOG = STATE / "touch.jsonl"

IN_RATE = 16000       # what the device sends
OUT_RATE = 24000      # what we send it
FRAME_MS = 60

SPEECH_RMS = 500      # 16-bit RMS above this is speech; tune against a real room
END_SILENCE_MS = 900  # this much quiet after speech ends an utterance
MIN_SPEECH_MS = 300   # shorter than this was a noise
MAX_UTTERANCE_S = 20

# A clip whose transcript is nothing but sound tags, like "(keyboard clacking)" or "[music]".
SOUND_TAGS = re.compile(r"^(?:\s|[.,!?…♪-]|\([^)]*\)|\[[^\]]*\]|\*[^*]*\*)*$")


def has_words(text: str) -> bool:
    return SOUND_TAGS.match(text) is None


def token_file() -> str:
    STATE.mkdir(exist_ok=True)
    p = STATE / "token"
    if not p.exists():
        p.write_text(secrets.token_urlsafe(24), encoding="utf-8")
    return p.read_text(encoding="utf-8").strip()


def lodestar_from_symposium(path: Path) -> tuple[str, str]:
    """The Lodestar route Symposium already holds on this box, rather than a second copy of its token."""
    text = path.read_text(encoding="utf-8")
    block = re.search(r"^lodestar:\s*\n((?:[ \t]+.*\n)+)", text, re.M)
    if not block:
        raise SystemExit(f"no lodestar block in {path}")
    url = re.search(r"^\s*url:\s*(\S+)", block.group(1), re.M)
    tok = re.search(r"^\s*bearer_token:\s*(\S+)", block.group(1), re.M)
    if not url or not tok:
        raise SystemExit(f"lodestar block in {path} lacks url or bearer_token")
    return url.group(1), tok.group(1).strip("'\"")


def wav_bytes(pcm: np.ndarray, rate: int) -> bytes:
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.astype(np.int16).tobytes())
    return out.getvalue()


def wav_to_opus(data: bytes) -> list[bytes]:
    """A WAV as the Opus packets the device plays: mono, 24 kHz, one packet per 60 ms."""
    enc = av.CodecContext.create("libopus", "w")
    enc.sample_rate = OUT_RATE
    enc.layout = "mono"
    enc.format = "s16"
    enc.bit_rate = 32000
    enc.options = {"frame_duration": str(FRAME_MS), "application": "voip"}
    enc.open()
    size = OUT_RATE * FRAME_MS // 1000
    resampler = av.AudioResampler(format="s16", layout="mono", rate=OUT_RATE)
    chunks = []
    with av.open(io.BytesIO(data), format="wav") as container:
        for frame in container.decode(audio=0):
            for r in resampler.resample(frame):
                chunks.append(r.to_ndarray().reshape(-1))
    for r in resampler.resample(None):
        chunks.append(r.to_ndarray().reshape(-1))
    pcm = np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.int16)
    packets = []
    for start in range(0, len(pcm), size):
        piece = pcm[start:start + size]
        if len(piece) < size:
            piece = np.pad(piece, (0, size - len(piece)))
        frame = av.AudioFrame.from_ndarray(piece.reshape(1, -1).astype(np.int16), format="s16", layout="mono")
        frame.sample_rate = OUT_RATE
        frame.pts = start
        packets += [bytes(p) for p in enc.encode(frame)]
    packets += [bytes(p) for p in enc.encode(None)]
    return packets


class Device:
    """One connected websocket: its audio decoder and the utterance being collected."""

    def __init__(self, ws, session_id):
        self.ws = ws
        self.session_id = session_id
        self.decoder = av.CodecContext.create("libopus", "r")
        self.decoder.sample_rate = IN_RATE
        self.decoder.layout = "mono"
        self.resampler = av.AudioResampler(format="s16", layout="mono", rate=IN_RATE)
        self.speaking = False
        self.last_words = time.monotonic()  # the channel hangs up after too long without any
        self.reset()

    def reset(self):
        self.pcm = []
        self.speech_ms = 0
        self.quiet_ms = 0
        self.in_speech = False

    def feed(self, packet: bytes):
        """Decode one frame. Returns a finished utterance's samples, or None."""
        chunks = []
        for frame in self.decoder.decode(av.Packet(packet)):
            # the Opus decoder hands back 48 kHz whatever it was told; bring it to the rate we count in
            for r in self.resampler.resample(frame):
                chunks.append(r.to_ndarray().reshape(-1))
        if not chunks:
            return None
        pcm = np.concatenate(chunks).astype(np.int16)
        ms = 1000 * len(pcm) / IN_RATE
        rms = float(np.sqrt(np.mean(np.square(pcm.astype(np.float64))))) if len(pcm) else 0.0

        if self.in_speech or rms >= SPEECH_RMS:
            self.pcm.append(pcm)
        if rms >= SPEECH_RMS:
            self.in_speech = True
            self.speech_ms += ms
            self.quiet_ms = 0
        elif self.in_speech:
            self.quiet_ms += ms

        total_ms = sum(len(p) for p in self.pcm) * 1000 / IN_RATE
        if self.in_speech and (self.quiet_ms >= END_SILENCE_MS or total_ms >= MAX_UTTERANCE_S * 1000):
            done = np.concatenate(self.pcm)
            enough = self.speech_ms >= MIN_SPEECH_MS
            self.reset()
            return done if enough else None
        return None


class Link:
    def __init__(self, args):
        self.args = args
        self.token = token_file()
        self.lodestar_url, self.lodestar_token = lodestar_from_symposium(Path(args.symposium_yaml).expanduser())
        self.device: Device | None = None
        self.http: ClientSession | None = None
        self.say_lock = asyncio.Lock()
        HEARD_DIR.mkdir(parents=True, exist_ok=True)

    def lodestar_headers(self):
        return {"Authorization": f"Bearer {self.lodestar_token}"}

    async def speak_wav(self, text: str, voice: str) -> bytes:
        async with self.http.post(f"{self.lodestar_url}/api/v1/speak", headers=self.lodestar_headers(),
                                  json={"text": text, "voice": voice, "holder": "seren-voice"},
                                  timeout=ClientTimeout(total=240)) as r:
            if r.status != 200:
                raise web.HTTPBadGateway(text=f"speak: {r.status} {(await r.text())[:200]}")
            return await r.read()

    async def hear(self, pcm: np.ndarray):
        name = time.strftime("%Y%m%d-%H%M%S")
        data = wav_bytes(pcm, IN_RATE)
        path = HEARD_DIR / f"{name}.wav"
        path.write_bytes(data)
        form = FormData()
        form.add_field("file", data, filename=f"{name}.wav", content_type="audio/wav")
        form.add_field("language", "en")
        form.add_field("holder", "seren-voice")
        text = ""
        transcribed = False
        try:
            async with self.http.post(f"{self.lodestar_url}/api/v1/hear", headers=self.lodestar_headers(), data=form,
                                      timeout=ClientTimeout(total=120)) as r:
                body = await r.json(content_type=None)
                text = (body.get("text") or "").strip() if isinstance(body, dict) else ""
                transcribed = r.status == 200
                if not transcribed:
                    log.warning("hear: %s %s", r.status, str(body)[:200])
        except Exception as e:
            log.warning("hear failed: %s", e)
        # Only a clip that was really transcribed can be called noise: a failed hear may hold words.
        if transcribed and not has_words(text):
            path.unlink(missing_ok=True)
            log.info("dropped a clip with no words (%.1fs)", len(pcm) / IN_RATE)
            return
        entry = {"t": time.time(), "wav": str(path), "seconds": round(len(pcm) / IN_RATE, 1), "text": text}
        with HEARD_LOG.open("a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
        log.info("heard (%ss): %s", entry["seconds"], text)
        d = self.device
        if d is not None and text:
            d.last_words = time.monotonic()
            await self.send(d, {"type": "stt", "text": text})

    async def send(self, d: Device, message: dict):
        await d.ws.send_str(json.dumps({"session_id": d.session_id, **message}))

    async def say(self, text: str, voice: str, emotion: str | None):
        wav = await self.speak_wav(text, voice)
        packets = await asyncio.to_thread(wav_to_opus, wav)
        async with self.say_lock:
            d = self.device
            if d is None:
                raise web.HTTPConflict(text="the device has no audio channel open: wake it first")
            d.speaking = True
            try:
                await self.send(d, {"type": "tts", "state": "start"})
                if emotion:
                    await self.send(d, {"type": "llm", "emotion": emotion, "text": ""})
                await self.send(d, {"type": "tts", "state": "sentence_start", "text": text})
                started = time.monotonic()
                for i, packet in enumerate(packets):
                    if not d.speaking:
                        break
                    await d.ws.send_bytes(packet)
                    # a little ahead of real time, so the device's queue never runs dry
                    lead = started + (i * FRAME_MS / 1000) - 0.2 - time.monotonic()
                    if lead > 0:
                        await asyncio.sleep(lead)
                remaining = started + (len(packets) * FRAME_MS / 1000) - time.monotonic()
                if remaining > 0 and d.speaking:
                    await asyncio.sleep(remaining)
                await self.send(d, {"type": "tts", "state": "stop"})
            finally:
                d.speaking = False
                d.last_words = time.monotonic()
                d.reset()
        return len(packets) * FRAME_MS / 1000

    # -- routes ---------------------------------------------------------------------------

    async def ota(self, request):
        """The check-in. No mqtt, no activation, no firmware: the websocket is all it gets."""
        host = self.args.advertise
        return web.json_response({
            "websocket": {"url": f"ws://{host}:{self.args.port}/voice", "token": self.token, "version": 1},
            "server_time": {"timestamp": int(time.time() * 1000), "timezone_offset": -time.timezone // 60},
        })

    async def voice(self, request):
        auth = request.headers.get("Authorization", "")
        if not secrets.compare_digest(auth, f"Bearer {self.token}"):
            return web.Response(status=401, text="bad token")
        ws = web.WebSocketResponse(max_msg_size=4 * 1024 * 1024, heartbeat=None)
        await ws.prepare(request)
        d = Device(ws, secrets.token_hex(4))
        self.device = d
        log.info("device connected: %s", request.headers.get("Device-Id", "?"))
        try:
            async for msg in ws:
                if msg.type == WSMsgType.BINARY:
                    if d.speaking:
                        continue
                    done = d.feed(msg.data)
                    if done is not None:
                        asyncio.create_task(self.hear(done))
                elif msg.type == WSMsgType.TEXT:
                    await self.on_json(d, json.loads(msg.data))
                elif msg.type == WSMsgType.ERROR:
                    log.warning("websocket error: %s", ws.exception())
        finally:
            if self.device is d:
                self.device = None
            log.info("device gone")
        return ws

    async def on_json(self, d: Device, m: dict):
        kind = m.get("type")
        if kind == "hello":
            log.info("hello: %s", json.dumps(m)[:300])
            await d.ws.send_str(json.dumps({
                "type": "hello", "transport": "websocket", "session_id": d.session_id,
                "audio_params": {"format": "opus", "sample_rate": OUT_RATE, "channels": 1, "frame_duration": FRAME_MS},
            }))
        elif kind == "listen":
            log.info("listen: %s %s", m.get("state"), m.get("text", ""))
            if m.get("state") == "start":
                d.last_words = time.monotonic()
                d.reset()
        elif kind == "abort":
            log.info("abort: %s", m.get("reason", ""))
            d.speaking = False
        else:
            log.info("%s: %s", kind, json.dumps(m)[:200])

    def authorized(self, request):
        auth = request.headers.get("Authorization", "")
        if not secrets.compare_digest(auth, f"Bearer {self.token}"):
            raise web.HTTPUnauthorized(text="bad token")

    async def say_route(self, request):
        self.authorized(request)
        body = await request.json()
        text = (body.get("text") or "").strip()
        if not text:
            raise web.HTTPBadRequest(text="no text")
        seconds = await self.say(text, body.get("voice") or self.args.voice, body.get("emotion"))
        return web.json_response({"ok": True, "seconds": seconds})

    async def status(self, request):
        self.authorized(request)
        return web.json_response({"ok": True, "device": self.device is not None,
                                  "speaking": bool(self.device and self.device.speaking)})

    async def watch_touch(self):
        """Poll the device's head touch counters and log each new pat, so I can feel them."""
        url = f"http://{self.args.device}:8080/api/touch"
        pats = presses = None
        while True:
            await asyncio.sleep(1.0)
            try:
                async with self.http.get(url, timeout=ClientTimeout(total=4)) as r:
                    t = await r.json(content_type=None)
            except Exception:
                continue
            if pats is not None and (t["pats"] < pats or t["presses"] < presses):
                pats = presses = None  # the device rebooted
            if pats is not None and (t["pats"] > pats or t["presses"] > presses):
                entry = {"t": time.time(), "new_pats": t["pats"] - pats, "new_presses": t["presses"] - presses}
                with TOUCH_LOG.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(entry) + "\n")
                log.info("touched: %s", entry)
            pats, presses = t["pats"], t["presses"]

    async def watch_silence(self):
        """Hang up when nobody has said a word for a while, so a forgotten tap can't leave the mic open."""
        while True:
            await asyncio.sleep(1.0)
            d = self.device
            if d is None or d.speaking or self.say_lock.locked():
                continue
            if time.monotonic() - d.last_words >= self.args.idle_close:
                log.info("no words for %ss: closing the channel", self.args.idle_close)
                await d.ws.close()

    async def on_start(self, app):
        self.http = ClientSession()
        self.touch_task = asyncio.create_task(self.watch_touch())
        self.silence_task = asyncio.create_task(self.watch_silence())

    async def on_stop(self, app):
        self.touch_task.cancel()
        self.silence_task.cancel()
        await self.http.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=7480)
    ap.add_argument("--advertise", required=True, help="the address the device can reach this box on")
    ap.add_argument("--voice", default="wren")
    ap.add_argument("--device", default="192.168.0.201", help="the robot, for head touch")
    ap.add_argument("--idle-close", type=float, default=60, help="seconds without words before hanging up")
    ap.add_argument("--symposium-yaml", default="~/seren/wren/apps/symposium/seren-symposium.yaml")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("aiohttp.access").setLevel(logging.WARNING)

    link = Link(args)
    app = web.Application(client_max_size=8 * 1024 * 1024)
    app.on_startup.append(link.on_start)
    app.on_cleanup.append(link.on_stop)
    app.router.add_route("*", "/ota/", link.ota)
    app.router.add_get("/voice", link.voice)
    app.router.add_post("/say", link.say_route)
    app.router.add_get("/status", link.status)
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
