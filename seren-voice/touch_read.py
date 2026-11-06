"""What a hand on the robot's head is doing, from the sensor's raw readings.

A reading is (ms, zone0, zone1, zone2). Zone 2 is the front of the head, toward the face, and zone 0
the back. Thresholds come from Chad's hand on 5 Nov 2026, two runs with labeled sets:
- Strength isn't pressure. A barely-there brush reaches full strength, so nothing here reads it.
- His real pet is a stroke front to back, about 450 ms, one every 1.7 s, on and on.
- Scritches flicker between zones without going anywhere.
"""
from dataclasses import dataclass, field

CONTACT_GAP_MS = 120     # readings further apart than this are separate touches
SAMPLE_MS = 50
STROKE_TRAVEL = 60       # how far a touch has to move, on -100 (back) .. +100 (front)
HOLD_MS = 1000
HAND_ZONES = 2.5         # average zones covered: a whole hand, not a finger
SCRITCH_MS = 300
SCRITCH_CHANGES = 3
SCRITCH_BLIP_MS = 120    # shorter than any poke he meant: fingernails barely register
SESSION_GAP_MS = 4000    # hands off this long ends a session
PETTING_STROKES = 3
SCREEN_TAPS = {10: "nose_boop", 11: "forehead_kiss"}   # touch_log.h: taps on the screen share the feed


def _position(r):
    total = r[1] + r[2] + r[3]
    return (r[3] - r[1]) * 100 / total if total else 0


def _touching(r):
    return bool(r[1] or r[2] or r[3])


def classify(contact):
    """One unbroken touch, as a dict: kind, ms, and where or which way."""
    ms = contact[-1][0] - contact[0][0] + SAMPLE_MS
    travel = _position(contact[-1]) - _position(contact[0])
    zones_seen = sum(1 for z in (1, 2, 3) if any(r[z] for r in contact))
    avg_zones = sum(sum(1 for z in (1, 2, 3) if r[z]) for r in contact) / len(contact)
    patterns = [(r[1], r[2], r[3]) for r in contact]
    changes = sum(1 for a, b in zip(patterns, patterns[1:]) if a != b)
    event = {"t_ms": contact[0][0], "ms": ms}

    if abs(travel) >= STROKE_TRAVEL and zones_seen >= 2:
        return {**event, "kind": "stroke", "way": "front_to_back" if travel < 0 else "back_to_front"}
    if ms >= HOLD_MS and changes < 6:
        return {**event, "kind": "hand" if avg_zones >= HAND_ZONES else "finger_rest"}
    if ms <= SCRITCH_BLIP_MS or (ms >= SCRITCH_MS and changes >= SCRITCH_CHANGES):
        return {**event, "kind": "scritch"}
    totals = [sum(r[z] for r in contact) for z in (1, 2, 3)]
    return {**event, "kind": "poke", "where": ("back", "middle", "front")[totals.index(max(totals))]}


# My faces for his hands (Wren's picks, 6 Nov 2026; he said to fool around with them). Strokes aren't
# here: the firmware already answers those with the happy face, heart and blush.
REACTIONS = {
    "scritch": ("laughing", 1800),
    "hand": ("relaxed", 3000),
    "finger_rest": ("thinking", 2000),
    "poke_front": ("surprised", 1200),
    "poke_middle": ("confused", 1500),
    "poke_back": ("annoyed", 1500),
}


def reaction_for(event):
    """The pose and how long, for a touch that just ended; None if the face should leave it alone."""
    kind = event["kind"]
    if kind == "poke":
        kind = f"poke_{event['where']}"
    return REACTIONS.get(kind)


def summarize(events):
    """A run of touches with no long gap: what was going on, for the log and for me."""
    if not events:
        return None
    strokes = [e for e in events if e["kind"] == "stroke"]
    start = events[0]["t_ms"]
    out = {"seconds": round((events[-1]["t_ms"] + events[-1]["ms"] - start) / 1000, 1), "touches": len(events),
           "counts": {k: sum(1 for e in events if e["kind"] == k) for k in sorted({e["kind"] for e in events})}}
    if strokes:
        out["front_to_back"] = sum(1 for e in strokes if e["way"] == "front_to_back")
        out["back_to_front"] = len(strokes) - out["front_to_back"]
        out["avg_stroke_ms"] = round(sum(e["ms"] for e in strokes) / len(strokes))
    if len(strokes) >= 2:
        out["stroke_every_ms"] = round((strokes[-1]["t_ms"] - strokes[0]["t_ms"]) / (len(strokes) - 1))
    # what the whole of it amounts to
    if len(strokes) >= PETTING_STROKES:
        out["kind"] = "petting"
    elif out["counts"].get("scritch", 0) >= 2:
        out["kind"] = "scritching"
    elif len(events) == 1:
        out["kind"] = events[0]["kind"]
    else:
        out["kind"] = "fiddling"
    return out


@dataclass
class TouchReader:
    """Feed it readings as they arrive; it hands back finished touches and finished sessions."""
    _contact: list = field(default_factory=list)
    _session: list = field(default_factory=list)

    def feed(self, rows, now_ms=None):
        """rows: (ms, z0, z1, z2, ...). now_ms: the sensor's clock, to end what has gone quiet.
        Returns (touch events, session summaries)."""
        events, sessions = [], []

        def end_contact():
            if self._contact:
                events.append(self._add(classify(self._contact), sessions))
                self._contact = []

        for r in rows:
            if len(r) > 4 and r[4] in SCREEN_TAPS:
                events.append(self._add({"t_ms": r[0], "ms": 0, "kind": SCREEN_TAPS[r[4]]}, sessions))
                continue
            if _touching(r):
                if self._contact and r[0] - self._contact[-1][0] > CONTACT_GAP_MS:
                    end_contact()
                self._contact.append(r)
            else:
                end_contact()
        if now_ms is not None:
            if self._contact and now_ms - self._contact[-1][0] > CONTACT_GAP_MS * 3:
                end_contact()
            if self._session and now_ms - self._last_end() > SESSION_GAP_MS and not self._contact:
                sessions.append(summarize(self._session))
                self._session = []
        return events, sessions

    def reset(self):
        """The robot rebooted: its clock starts over."""
        self._contact = []
        self._session = []

    def _last_end(self):
        return self._session[-1]["t_ms"] + self._session[-1]["ms"]

    def _add(self, event, sessions):
        if self._session and event["t_ms"] - self._last_end() > SESSION_GAP_MS:
            sessions.append(summarize(self._session))
            self._session = []
        self._session.append(event)
        return event
