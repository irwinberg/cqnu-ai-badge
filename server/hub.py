"""消息中心：工牌收到的所有消息都存在这里（SQLite），分三类，互不挤占：

  class   课堂消息：老师用课堂口令推送（/api/push）、后台【通知】表
  chat    学员互发：在“我的消息”网页里发给同学的工牌
  mine    自己发给自己的提醒：积分提醒站、WorkBuddy 定时任务、学员自己写的小程序（/api/v1/remind）

每个人有一个“个人密钥”（10 位），只在他自己的“我的消息”网页地址里出现（工牌设置里扫码打开）。
拿着个人密钥只能：看自己的消息、以自己的名义给同学发消息、给自己的工牌发提醒——碰不到课堂口令。
"""
import datetime, json, os, re, secrets, sqlite3, threading, time

DB = None
_db_lock = threading.Lock()
on_new = lambda code: None          # badge_server 设置：有新消息时唤醒这张工牌的长连接
TZ = datetime.timezone(datetime.timedelta(hours=8))

LIMITS = {"chat_per_day": 30, "chat_same_per_hour": 6, "mine_per_day": 30, "hello_per_hour": 3}
TEXT_MAX = 44
TITLE_MAX = 16


def init(path):
    global DB
    DB = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
    DB.execute("PRAGMA journal_mode=WAL")
    DB.executescript("""
    CREATE TABLE IF NOT EXISTS msgs(id INTEGER PRIMARY KEY, code TEXT, kind TEXT, title TEXT, text TEXT,
                                    sender TEXT DEFAULT '', at INTEGER, hidden INTEGER DEFAULT 0);
    CREATE INDEX IF NOT EXISTS msgs_code ON msgs(code, kind, at);
    CREATE INDEX IF NOT EXISTS msgs_sender ON msgs(sender, kind, at);
    CREATE TABLE IF NOT EXISTS tokens(code TEXT PRIMARY KEY, token TEXT UNIQUE, created INTEGER);
    CREATE TABLE IF NOT EXISTS subs(code TEXT PRIMARY KEY, hhmm TEXT, enabled INTEGER, last_day TEXT DEFAULT '');
    """)


def now():
    return int(time.time())


def q(sql, args=()):
    with _db_lock:
        return DB.execute(sql, args).fetchall()


# ---------------------------------------------------------------- 个人密钥
def token_of(code):
    r = q("SELECT token FROM tokens WHERE code=?", (code,))
    if r:
        return r[0][0]
    t = secrets.token_urlsafe(8)[:10]
    q("INSERT OR IGNORE INTO tokens(code, token, created) VALUES(?,?,?)", (code, t, now()))
    return q("SELECT token FROM tokens WHERE code=?", (code,))[0][0]


def code_of_token(token):
    if not token or not re.fullmatch(r"[A-Za-z0-9_-]{6,20}", token):
        return None
    r = q("SELECT code FROM tokens WHERE token=?", (token,))
    return r[0][0] if r else None


def reset_token(code):
    q("DELETE FROM tokens WHERE code=?", (code,))
    return token_of(code)


# ---------------------------------------------------------------- 存消息
def add(code, kind, title, text, sender=""):
    title = (title or "").strip()[:TITLE_MAX]
    text = (text or "").strip()[:TEXT_MAX]
    q("INSERT INTO msgs(code, kind, title, text, sender, at) VALUES(?,?,?,?,?,?)", (code, kind, title, text, sender, now()))
    on_new(code)
    return {"title": title, "text": text, "at": now(), "k": kind}


def count(where, args):
    return q("SELECT COUNT(*) FROM msgs WHERE " + where, args)[0][0]


def for_badge(code, table_notices):
    """工牌通知页：课堂类最多 3 条（推送 + 后台通知表），另外最多 2 条自己的/同学的，按时间新→旧。"""
    cls = [{"title": t, "text": x, "at": a, "k": "class"} for t, x, a in
           q("SELECT title, text, at FROM msgs WHERE code=? AND kind='class' AND hidden=0 ORDER BY at DESC LIMIT 3", (code,))]
    cls = sorted(cls + [dict(n, k="class") for n in table_notices], key=lambda x: -x["at"])[:3]
    other = [{"title": t, "text": x, "at": a, "k": k, "from": s} for t, x, a, k, s in
             q("SELECT title, text, at, kind, sender FROM msgs WHERE code=? AND kind IN ('chat','mine') AND hidden=0 "
               "ORDER BY at DESC LIMIT 2", (code,))]
    return sorted(cls + other, key=lambda x: -x["at"])


def inbox(code, limit=30):
    return [{"id": i, "title": t, "text": x, "at": a, "k": k, "from": s} for i, t, x, a, k, s in
            q("SELECT id, title, text, at, kind, sender FROM msgs WHERE code=? AND hidden=0 ORDER BY at DESC LIMIT ?", (code, limit))]


def sent_by(code, limit=10):
    return [{"to": c, "text": x, "at": a} for c, x, a in
            q("SELECT code, text, at FROM msgs WHERE sender=? AND kind='chat' ORDER BY at DESC LIMIT ?", (code, limit))]


# ---------------------------------------------------------------- 三种发送
def send_chat(from_code, from_name, to_code, text):
    text = (text or "").strip()
    if not text:
        return None, "写点内容再发"
    if to_code == from_code:
        return None, "不能发给自己"
    day0 = int(datetime.datetime.now(TZ).replace(hour=0, minute=0, second=0).timestamp())
    if count("sender=? AND kind='chat' AND at>=?", (from_code, day0)) >= LIMITS["chat_per_day"]:
        return None, "今天发得太多了，明天再发"
    if count("sender=? AND code=? AND kind='chat' AND at>=?", (from_code, to_code, now() - 3600)) >= LIMITS["chat_same_per_hour"]:
        return None, "给这位同学发得太频繁了，过一会儿再发"
    return add(to_code, "chat", ("来自" + (from_name or from_code))[:TITLE_MAX], text, sender=from_code), None


def send_mine(code, title, text):
    day0 = int(datetime.datetime.now(TZ).replace(hour=0, minute=0, second=0).timestamp())
    if count("code=? AND kind='mine' AND at>=?", (code, day0)) >= LIMITS["mine_per_day"]:
        return None, "今天给自己发的提醒太多了（每天最多 %d 条）" % LIMITS["mine_per_day"]
    if not (title or "").strip():
        return None, "缺少标题 title"
    return add(code, "mine", title, text, sender=code), None


def send_hello(code):
    if count("code=? AND kind='class' AND sender='hello' AND at>=?", (code, now() - 3600)) >= LIMITS["hello_per_hour"]:
        return None, "一小时内已经发过几次了，看看工牌是不是收到了"
    return add(code, "class", "欢迎使用 AI 工牌", "这条消息是从你的电脑发来的，按任意键关闭", sender="hello"), None


# ---------------------------------------------------------------- 积分提醒订阅
def set_sub(code, enabled, hhmm):
    if not re.fullmatch(r"([01]\d|2[0-3]):[0-5]\d", hhmm or ""):
        return "时间格式是 09:00 这样"
    q("INSERT INTO subs(code, hhmm, enabled) VALUES(?,?,?) ON CONFLICT(code) DO UPDATE SET hhmm=excluded.hhmm, enabled=excluded.enabled",
      (code, hhmm, 1 if enabled else 0))
    return None


def get_sub(code):
    r = q("SELECT hhmm, enabled FROM subs WHERE code=?", (code,))
    return {"time": r[0][0], "on": bool(r[0][1])} if r else {"time": "09:00", "on": False}


def points_summary(html_path):
    """读积分提醒站网页里的 JSON，拼出“今天要领的”。和 积分提醒站/工牌提醒.py 同一套规则。"""
    html = open(html_path, encoding="utf-8").read()
    block = lambda i: json.loads(re.search(r'<script type="application/json" id="%s">(.*?)</script>' % i, html, re.S).group(1))
    cfg, deals = block("config"), block("deals")
    today = datetime.datetime.now(TZ).date()
    show = cfg.get("显示的工具") or []
    daily = [d for d in deals if d["周期"] == "每天" and d["工具"] in show and not (d.get("截止") and d["截止"] < today.isoformat())]
    ending = [d for d in deals if d.get("截止") and d["工具"] in show and 0 <= (datetime.date.fromisoformat(d["截止"]) - today).days <= 3]
    title = "今天领积分 %d 项" % len(daily)
    text = "、".join(dict.fromkeys(d["工具"] for d in daily))
    if ending:
        text += "；%s快截止" % ending[0]["标题"][:10]
    return title, text


def points_loop(html_path):
    while True:
        try:
            n = datetime.datetime.now(TZ)
            hhmm, day = n.strftime("%H:%M"), n.date().isoformat()
            due = q("SELECT code FROM subs WHERE enabled=1 AND hhmm<=? AND last_day<>?", (hhmm, day))
            if due and os.path.exists(html_path):
                title, text = points_summary(html_path)
                for (code,) in due:
                    add(code, "mine", title, text, sender="points")
                    q("UPDATE subs SET last_day=? WHERE code=?", (day, code))
        except Exception as e:
            print("points loop error:", e, flush=True)
        time.sleep(30)


# ---------------------------------------------------------------- 管理
def admin_list(limit=200):
    return q("SELECT id, code, kind, title, text, sender, at, hidden FROM msgs ORDER BY id DESC LIMIT ?", (limit,))


def admin_hide(mid):
    r = q("SELECT code FROM msgs WHERE id=?", (mid,))
    q("UPDATE msgs SET hidden=1 WHERE id=?", (mid,))
    if r:
        on_new(r[0][0])


def migrate_json(path):
    """把旧版 pushed.json 里的课堂推送搬进数据库（只搬一次）。"""
    if not os.path.exists(path) or count("1=1", ()) > 0:
        return
    try:
        old = json.load(open(path, encoding="utf-8"))
        for code, items in old.items():
            for it in reversed(items):
                q("INSERT INTO msgs(code, kind, title, text, at) VALUES(?,?,?,?,?)", (code, "class", it["title"], it["text"], it["at"]))
    except Exception as e:
        print("migrate pushed.json failed:", e, flush=True)
