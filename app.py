from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel
import sqlite3
from datetime import datetime, timezone
from typing import List, Set

app = FastAPI()
templates = Jinja2Templates(directory="templates")

DB_PATH = "parking.db"

def init_db():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts TEXT NOT NULL,
            device_id TEXT,
            uid TEXT NOT NULL,
            action TEXT NOT NULL,     -- IN / OUT
            minutes INTEGER DEFAULT 0,
            money INTEGER DEFAULT 0,
            slot1 INTEGER DEFAULT 0,
            slot2 INTEGER DEFAULT 0,
            slot3 INTEGER DEFAULT 0,
            slot4 INTEGER DEFAULT 0,
            free INTEGER DEFAULT 0
        )
    """)
    conn.commit()
    conn.close()

init_db()

class ParkingEventIn(BaseModel):
    device_id: str | None = "esp32_parking_01"
    uid: str
    action: str              # "IN" or "OUT"
    minutes: int = 0
    money: int = 0
    slot1: int = 0
    slot2: int = 0
    slot3: int = 0
    slot4: int = 0
    free: int = 0

def insert_event(e: ParkingEventIn, ts: str):
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    # Xóa lịch sử quá 7 ngày
    cur.execute("DELETE FROM events WHERE datetime(ts) < datetime('now', '-7 days')")
    # Thêm sự kiện mới
    cur.execute("""
        INSERT INTO events (ts, device_id, uid, action, minutes, money, slot1, slot2, slot3, slot4, free)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (ts, e.device_id, e.uid, e.action, e.minutes, e.money, e.slot1, e.slot2, e.slot3, e.slot4, e.free))
    conn.commit()
    conn.close()

def fetch_latest(limit: int = 200):
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()
    cur.execute("SELECT * FROM events ORDER BY datetime(ts) DESC LIMIT ?", (limit,))
    rows = [dict(r) for r in cur.fetchall()]
    conn.close()
    return rows

def fetch_total_money_today():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()
    today = datetime.now().strftime("%Y-%m-%d")
    cur.execute("""
        SELECT SUM(money) FROM events
        WHERE action='OUT' AND DATE(ts) = ?
    """, (today,))
    result = cur.fetchone()
    conn.close()
    return result[0] if result and result[0] is not None else 0

# --- WebSocket manager ---
clients: Set[WebSocket] = set()

async def broadcast_json(payload: dict):
    dead = []
    for ws in clients:
        try:
            await ws.send_json(payload)
        except Exception:
            dead.append(ws)
    for ws in dead:
        clients.discard(ws)

@app.get("/", response_class=HTMLResponse)
async def home(request: Request):
    total_money = fetch_total_money_today()
    return templates.TemplateResponse("index.html", {"request": request, "total_money": total_money})

@app.get("/api/events")
def api_events(limit: int = 200):
    limit = min(max(limit, 1), 2000)
    return fetch_latest(limit)

@app.post("/api/event")
async def api_event(e: ParkingEventIn):
    # timestamp ISO8601_strip_millis
    ts = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
    # normalize action
    action = e.action.strip().upper()
    if action not in ("IN", "OUT"):
        return {"ok": False, "error": "action must be IN or OUT"}
    e.action = action

    insert_event(e, ts)

    payload = {"type": "event:new", "data": {**e.model_dump(), "ts": ts}}
    await broadcast_json(payload)
    return {"ok": True}

@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    clients.add(ws)
    try:
        while True:
            # keep alive (client không cần gửi gì)
            await ws.receive_text()
    except WebSocketDisconnect:
        clients.discard(ws)
