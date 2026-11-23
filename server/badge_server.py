#!/usr/bin/env python3
"""AI 工牌同步服务（v2）：定时从飞书多维表格读数据，按工牌码给每张工牌返回一份精简 JSON。

只用 Python 标准库；通过本机已登录的 lark-cli 读写表（user 身份）。
安全：
  - /api/badge 必须带 X-Badge-Key（固件里写入同一把密钥），否则 403；
  - 只按工牌码返回这张工牌需要看的数据，不返回手机号，没有整表导出接口；
  - 局域网自动发现（UDP 8788）：工牌广播随机数，本服务用密钥回一个 HMAC，工牌校验后才认这台电脑。
运行：python3 badge_server.py
接口：GET /api/badge?code=F6E770&bat=97&ver=2[&fresh=1]   GET /healthz
      GET|POST /api/push?key=课堂口令&code=工牌码&title=标题&text=内容（config.json 里配了 push_key 才开放）
      GET /api/wait?code=&seq=  工牌长连接等新消息：有新推送马上返回，否则最多等 t 秒（默认 12）
消息中心（hub.py，v4）：
      GET  /m/<个人密钥>            “我的消息”网页（工牌设置里扫码打开）
      GET  /api/v1/me?token=         我的消息、同学名单、积分提醒设置
      POST /api/v1/send   {token,to,text}       给同学的工牌发消息（限频）
      POST /api/v1/remind {token,title,text}    给自己的工牌发提醒（积分站、WorkBuddy 定时任务、学员自己的程序用）
      POST /api/v1/points {token,on,time}       每天积分提醒
      GET  /api/v1/hello?code=       配置向导第 5 步：给这张工牌发一条欢迎消息（每小时最多 3 次，不需要口令）
      GET  /admin?key=管理口令        最近 200 条消息，可隐藏
自助登记：工牌扫码填【工牌登记】表单；同一个工牌码以最后一次登记为准，覆盖【人员】表里的姓名/身份/单位/学号/头像。
"""
import hashlib, hmac, html as htmlmod, json, os, re, socket, subprocess, sys, threading, time, urllib.parse
import hub
from datetime import datetime, timezone, timedelta
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = json.load(open(os.path.join(HERE, "config.json"), encoding="utf-8"))
TZ = timezone(timedelta(hours=8))
TABLES = ["人员", "课表", "作业", "作业提交", "作品", "评委打分", "观众投票", "通知", "Wi-Fi", "工牌", "工牌登记"]
FAST_TABLES = ["作业提交", "评委打分", "观众投票", "作品", "工牌登记"]          # 交作业/打分/投票后要尽快看到的
AVATARS = ["01 银发型男", "02 夹克大叔", "03 眼镜老师", "04 酷少年", "05 卫衣女生", "06 西装女士", "07 棒球帽"]
DISC_PORT = int(CFG.get("discovery_port", 8788))
CODE_RE = re.compile(r"[0-9A-F]{6}")
Q = urllib.parse.quote

_cache = {"tables": {}, "at": 0, "error": None}
_lock = threading.Lock()
_seen = {}          # 工牌码 -> {"first","last","bat","ver"}（本次运行里见过的工牌）
_written = {}       # 工牌码 -> {"t","bat"}（上次写进【工牌】表的时间和电量）
_last_fast = [0.0]
_pushed = {}        # 工牌码 -> [{"title","text","at"}]（课堂上从电脑推到工牌的通知，只在内存和 pushed.json 里）
PUSH_FILE = os.path.join(HERE, "pushed.json")
_cv = threading.Condition()   # 新推送到达时唤醒等着的工牌
_seq = {}                     # 工牌码 -> 推送序号（每推一条 +1）
try:
    _pushed.update(json.load(open(PUSH_FILE, encoding="utf-8")))
except Exception:
    pass


# ---------------------------------------------------------------- 飞书
def cli(*args, timeout=60):
    out = subprocess.run([CFG.get("lark_cli", "lark-cli"), "base", *args, "--base-token", CFG["base_token"], "--as", CFG.get("lark_as", "user")],
                         capture_output=True, text=True, timeout=timeout).stdout
    return json.loads(out[out.index("{"):])


def fetch_table(name):
    rows, offset = [], 0
    while True:
        d = cli("+record-list", "--table-id", name, "--format", "json", "--limit", "200", "--offset", str(offset))
        if not d.get("ok"):
            raise RuntimeError(f"读表失败 {name}: {d.get('error')}")
        data = d["data"]
        for rid, vals in zip(data["record_id_list"], data["data"]):
            r = dict(zip(data["fields"], vals)); r["_id"] = rid
            rows.append(r)
        if not data.get("has_more"):
            return rows
        offset += 200


def refresh_tables(names):
    t = {n: fetch_table(n) for n in names}
    with _lock:
        _cache["tables"].update(t)
        _cache["at"] = time.time()
        _cache["error"] = None


def refresh_once():
    refresh_tables(TABLES)


def refresh_loop():
    while True:
        try:
            refresh_once()
        except Exception as e:  # 读失败时保留上一次的缓存
            with _lock:
                _cache["error"] = str(e)
            print("refresh error:", e, file=sys.stderr, flush=True)
        time.sleep(CFG.get("refresh_seconds", 30))


def refresh_fast():
    """工牌要求"马上看到结果"时，只重读几张变化快的表；最多 6 秒一次。"""
    with _lock:
        if time.time() - _last_fast[0] < 6:
            return
        _last_fast[0] = time.time()
    try:
        refresh_tables(FAST_TABLES)
    except Exception as e:
        print("fast refresh error:", e, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- 工具
def txt(v):
    if v is None: return ""
    if isinstance(v, list):
        return ",".join(txt(x) for x in v)
    if isinstance(v, dict):
        return str(v.get("text") or v.get("name") or v.get("link") or "")
    s = str(v)
    if s.startswith("[") and "](" in s:  # markdown 链接
        return s[1:s.index("](")]
    return s

def opts(v):
    if v is None: return []
    if isinstance(v, list): return [txt(x) for x in v]
    return [txt(v)]

def num(v, d=0):
    try:
        if isinstance(v, list): v = v[0] if v else d
        return int(float(v))
    except Exception:
        return d

def ts(v):
    s = txt(v)
    if not s: return 0
    try:
        return int(datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp())
    except Exception:
        return 0

def link_ids(v):
    return [x.get("id") for x in (v or []) if isinstance(x, dict)]

def code_of(v):
    return txt(v).strip().upper()

def avatar_index(v):
    o = opts(v)
    return AVATARS.index(o[0]) if o and o[0] in AVATARS else 0

def form(key):
    return CFG["forms"][key]

def hidden(field, value):
    f = Q(field)
    return f"prefill_{f}={value}&hide_{f}=1"


# ---------------------------------------------------------------- 组装
REG_KEYS = ["姓名", "身份", "班级/单位", "学号/编号", "头像"]


def persons(T):
    """【人员】表 + 【工牌登记】表（自助登记）。同一工牌码以最后一次登记为准；登记内容覆盖人员表里的同名字段。"""
    regs = {}
    for r in T.get("工牌登记", []):
        c = code_of(r.get("码"))
        if CODE_RE.fullmatch(c) and txt(r.get("姓名")).strip():
            regs[c] = r
    out, used = [], set()
    for p in T.get("人员", []):
        c = code_of(p.get("工牌码"))
        r = regs.get(c) if c else None
        if r:
            p = dict(p)
            for k in REG_KEYS:
                if r.get(k) not in (None, "", []):
                    p[k] = r[k]
            used.add(c)
        out.append(p)
    for c, r in regs.items():
        if c not in used:
            q = {k: r.get(k) for k in REG_KEYS}
            q.update({"工牌码": c, "_id": r["_id"], "_self": True})
            out.append(q)
    return out


def build(code, tables=None, now=None):
    code = (code or "").strip().upper()
    with _lock:
        T = tables if tables is not None else _cache["tables"]
    now = int(now or time.time())
    out = {"v": 2, "now": now, "code": code}
    if not T:
        out["bound"] = False; out["err"] = "server_warming_up"; return out

    out["wifi"] = [{"s": txt(w.get("名称")).strip(), "p": txt(w.get("密码"))}
                   for w in T.get("Wi-Fi", []) if w.get("启用") and txt(w.get("名称")).strip()][:4]
    out["tpl"] = {
        "hw": form("交作业") + "?" + hidden("码", Q(code)) + "&" + hidden("题", "{id}"),
        "vote": form("投票") + "?" + hidden("品", "{id}") + "&" + hidden("码", Q(code)),
    }

    if "工牌登记" in CFG["forms"]:
        out["self_url"] = form("工牌登记") + "?" + hidden("码", Q(code))
    if CFG.get("public_base"):
        out["my_url"] = CFG["public_base"].rstrip("/") + "/m/" + hub.token_of(code)
    people = persons(T); works = T["作品"]
    me = next((p for p in people if code and code_of(p.get("工牌码")) == code), None)
    approved = [w for w in works if "已通过" in opts(w.get("审核状态"))]
    ranked = sorted(approved, key=lambda w: -num(w.get("总积分")))

    walkin = None
    if not me:
        walkin = next((w for w in works if code and code_of(w.get("码")) == code), None)
        if not walkin or "已通过" not in opts(walkin.get("审核状态")):
            out["bound"] = False
            out["reg_url"] = form("参赛报名") + "?" + hidden("码", Q(code))
            if walkin:
                out["reg"] = {"work": txt(walkin.get("作品名")), "state": (opts(walkin.get("审核状态")) or ["待审核"])[0]}
            out.update(vis_part(T, code, approved))
            out["notices"] = with_pushed(code, notices(T, ["游客"]))
            return out

    out["bound"] = True
    if me:
        roles = opts(me.get("身份"))
        out.update(name=txt(me.get("姓名")), org=txt(me.get("班级/单位")), sid=txt(me.get("学号/编号")),
                   avatar=avatar_index(me.get("头像")))
    else:
        roles = ["参赛选手"]
        out.update(name=txt(walkin.get("报名人姓名")), org="现场报名", sid="", avatar=3)

    modes = []
    if "学生" in roles: modes.append("stu")
    if "教师" in roles: modes.append("tea")
    my_work = walkin if not me else next((w for w in approved if me["_id"] in link_ids(w.get("成员"))), None)
    if my_work: modes.append("con")
    modes.append("vis")
    out["modes"] = modes

    if "stu" in modes: out["stu"] = stu_part(T, me, code, now)
    if "tea" in modes: out["tea"] = tea_part(T, me, now)
    if "con" in modes: out["con"] = con_part(T, my_work, ranked)
    out.update(vis_part(T, code, approved))
    names = {"stu": "学生", "tea": "教师", "con": "参赛选手", "vis": "游客"}
    out["notices"] = with_pushed(code, notices(T, [names[m] for m in modes] + (["评委"] if "评委" in roles else [])))
    return out


def classes_for(T, pred, now):
    """今天和明天的课（界面上"今天的课"只显示今天的，首页"下一节"可以是明天的）。"""
    day0 = datetime.fromtimestamp(now, TZ).replace(hour=0, minute=0, second=0, microsecond=0)
    lo, hi = int(day0.timestamp()), int((day0 + timedelta(days=2)).timestamp())
    res = []
    for c in T["课表"]:
        t = ts(c.get("上课时间"))
        if not (lo <= t < hi) or not pred(c): continue
        res.append({"t": t, "end": ts(c.get("下课时间")), "name": txt(c.get("课程名"))[:16],
                    "place": txt(c.get("地点"))[:10], "tip": txt(c.get("课前提示"))[:16]})
    return sorted(res, key=lambda x: x["t"])[:6]


def stu_part(T, me, code, now):
    org = txt(me.get("班级/单位"))
    cls = classes_for(T, lambda c: not txt(c.get("上课班级")) or txt(c.get("上课班级")) == org, now)
    subs = [s for s in T["作业提交"] if code_of(s.get("码")) == code]
    hw = []
    for h in T["作业"]:
        if not h.get("已发布"): continue
        hid = txt(h.get("题号"))
        mine = [s for s in subs if txt(s.get("题")) == hid]
        scored = [s for s in mine if s.get("分数") not in (None, "")]
        st = 2 if scored else (1 if mine else 0)
        item = {"id": Q(hid), "name": txt(h.get("作业名"))[:16], "due": ts(h.get("截止时间")), "st": st}
        if scored:
            item["score"] = num(scored[-1].get("分数")); item["note"] = txt(scored[-1].get("评语"))[:20]
        hw.append(item)
    hw.sort(key=lambda x: (x["st"] != 0, x["due"] or 1 << 40))
    total = sum(x.get("score", 0) for x in hw)
    return {"classes": cls, "hw": hw[:6], "score": total, "done": sum(1 for x in hw if x["st"]), "total": len(hw)}


def tea_part(T, me, now):
    cls = classes_for(T, lambda c: me["_id"] in link_ids(c.get("授课老师")), now)
    students = [p for p in persons(T) if "学生" in opts(p.get("身份"))]
    pub = [h for h in T["作业"] if h.get("已发布")]
    prog = None
    if pub:
        h = sorted(pub, key=lambda x: ts(x.get("截止时间")))[-1]
        hid = txt(h.get("题号"))
        subs = [s for s in T["作业提交"] if txt(s.get("题")) == hid]
        codes = {code_of(s.get("码")) for s in subs}
        missing = [txt(p.get("姓名")) for p in students if code_of(p.get("工牌码")) not in codes]
        prog = {"name": txt(h.get("作业名"))[:16], "sub": len(codes), "total": len(students),
                "graded": sum(1 for s in subs if s.get("分数") not in (None, "")),
                "todo": sum(1 for s in subs if s.get("分数") in (None, "")), "missing": missing[:12]}
    return {"classes": cls, "hw": prog}


def con_part(T, work, ranked):
    wn = txt(work.get("作品名"))
    wid = txt(work.get("作品号"))
    rank = next((i + 1 for i, w in enumerate(ranked) if w["_id"] == work["_id"]), len(ranked))
    score = num(work.get("总积分"))
    gap = num(ranked[rank - 2].get("总积分")) - score if rank > 1 else 0
    people = {p["_id"]: p for p in persons(T)}
    def av(w):
        m = [people[i] for i in link_ids(w.get("成员")) if i in people]
        return avatar_index(m[0].get("头像")) if m else 3
    top = [{"r": i + 1, "n": txt(w.get("作品名"))[:10], "s": num(w.get("总积分")), "av": av(w),
            "me": w["_id"] == work["_id"]} for i, w in enumerate(ranked[:5])]
    js = sorted([j for j in T["评委打分"] if txt(j.get("品")) == wid], key=lambda j: -ts(j.get("提交时间")))
    recent = [{"s": num(j.get("分数")), "d": "评委打分", "t": ts(j.get("提交时间"))} for j in js[:4]]
    return {"work": wn[:16], "no": wid, "score": score, "rank": rank, "of": len(ranked),
            "gap": max(gap, 0), "top": top, "recent": recent, "votes": num(work.get("票数")),
            "judge_url": form("评委打分") + "?" + hidden("品", Q(wid))}


def vis_part(T, code, approved):
    voted = {txt(v.get("品")) for v in T["观众投票"] if code and code_of(v.get("码")) == code}
    ws = sorted(approved, key=lambda w: (txt(w.get("展台号")) or "99", txt(w.get("作品号"))))
    items = [{"id": Q(txt(w.get("作品号"))), "no": (txt(w.get("展台号")) or txt(w.get("作品号")))[:6],
              "name": txt(w.get("作品名"))[:16], "intro": txt(w.get("作品简介"))[:30],
              "votes": num(w.get("票数")), "voted": txt(w.get("作品号")) in voted}
             for w in ws if w.get("开放投票")]
    return {"vis": {"works": items[:12], "voted": len(voted)}}


def notices(T, roles):
    res = []
    for n in T["通知"]:
        if not n.get("已发布"): continue
        to = opts(n.get("发送对象"))
        if "全体" in to or any(r in to for r in roles):
            res.append({"title": txt(n.get("标题"))[:16], "text": txt(n.get("内容"))[:44], "at": ts(n.get("发布时间"))})
    return sorted(res, key=lambda x: -x["at"])[:3]


def bump(code):
    """有新消息：序号 +1，唤醒这张工牌挂着的长连接。"""
    with _cv:
        _seq[code] = _seq.get(code, 0) + 1
        _cv.notify_all()


hub.on_new = bump


def push_notice(code, title, text):
    """课堂推送（老师用课堂口令）。存进消息中心，类型 class。"""
    return hub.add(code, "class", title, text, sender="teacher")


def with_pushed(code, base):
    """工牌通知页：课堂类 3 条（推送 + 通知表）+ 同学/自己的提醒 2 条，互不挤占。"""
    return hub.for_badge(code, base)


def people_list(exclude=""):
    with _lock:
        T = _cache["tables"]
    if not T:
        return []
    res = [{"code": code_of(p.get("工牌码")), "name": txt(p.get("姓名"))[:12], "org": txt(p.get("班级/单位"))[:12]}
           for p in persons(T) if CODE_RE.fullmatch(code_of(p.get("工牌码")) or "")]
    return sorted([r for r in res if r["code"] != exclude and r["name"]], key=lambda r: (r["org"], r["name"]))


def name_of(code):
    return next((p["name"] for p in people_list() if p["code"] == code), "")


# ---------------------------------------------------------------- 【工牌】表：自动登记、电量、最近上线
def note_badge(code, bat, ver):
    now = time.time()
    with _lock:
        s = _seen.setdefault(code, {"first": now})
        s.update(last=now, bat=bat, ver=ver)


def fmt_dt(t):
    return datetime.fromtimestamp(t, TZ).strftime("%Y-%m-%d %H:%M:%S")


def registry_sync():
    with _lock:
        seen = {k: dict(v) for k, v in _seen.items()}
        rows = list(_cache["tables"].get("工牌", []))
        loaded = "工牌" in _cache["tables"]
    if not loaded or not seen:
        return
    by_code = {code_of(r.get("码")): r for r in rows}
    new = [c for c in seen if c not in by_code]
    if new:
        body = {"fields": ["码", "首次上线", "最近上线", "电量", "版本"],
                "rows": [[c, fmt_dt(seen[c]["first"]), fmt_dt(seen[c]["last"]),
                          seen[c]["bat"] if seen[c]["bat"] is not None else None, seen[c]["ver"]] for c in new]}
        d = cli("+record-batch-create", "--table-id", "工牌", "--json", json.dumps(body, ensure_ascii=False))
        print(("登记新工牌 " if d.get("ok") else "登记工牌失败 ") + ",".join(new), flush=True)
        for c in new:
            _written[c] = {"t": seen[c]["last"], "bat": seen[c]["bat"]}
        refresh_tables(["工牌"])
    for c, s in seen.items():
        r = by_code.get(c)
        if not r:
            continue
        w = _written.get(c)
        bat_moved = w and s["bat"] is not None and w["bat"] is not None and abs(s["bat"] - w["bat"]) >= 5
        if w is None or s["last"] - w["t"] >= 300 or bat_moved:
            patch = {"最近上线": fmt_dt(s["last"]), "版本": s["ver"]}
            if s["bat"] is not None:
                patch["电量"] = s["bat"]
            cli("+record-batch-update", "--table-id", "工牌", "--json",
                json.dumps({"record_id_list": [r["_id"]], "patch": patch}, ensure_ascii=False))
            _written[c] = {"t": s["last"], "bat": s["bat"]}
            time.sleep(0.5)


def registry_loop():
    while True:
        time.sleep(15)
        try:
            registry_sync()
        except Exception as e:
            print("registry error:", e, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- 局域网自动发现
def discovery_loop():
    key = CFG["badge_key"].encode()
    port = int(CFG.get("port", 8787))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", DISC_PORT))
    pat = re.compile(rb"CQBADGE1 ([0-9a-f]{16})")
    while True:
        try:
            data, addr = sock.recvfrom(256)
            m = pat.fullmatch(data.strip())
            if not m:
                continue
            nonce = m.group(1).decode()
            print("工牌询问同步电脑：", addr[0], flush=True)
            mac = hmac.new(key, f"{nonce}|{port}".encode(), hashlib.sha256).hexdigest()[:32]
            sock.sendto(f"CQBADGE1 {port} {mac}".encode(), addr)
        except Exception as e:
            print("discovery error:", e, file=sys.stderr, flush=True)
            time.sleep(1)


# ---------------------------------------------------------------- HTTP
def parse_url(raw):
    """http.server 按 latin-1 解请求行；curl 直接带中文（没做 URL 编码）时要还原成 UTF-8。"""
    try:
        raw = raw.encode("latin-1").decode("utf-8")
    except (UnicodeDecodeError, UnicodeEncodeError):
        pass
    u = urllib.parse.urlparse(raw)
    return u, urllib.parse.parse_qs(u.query)


class H(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        b = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)

    def _html(self, code, body):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)

    def _body(self, q):
        raw = self.rfile.read(min(int(self.headers.get("Content-Length") or 0), 4096)).decode("utf-8")
        body = json.loads(raw) if raw.strip().startswith("{") else {k: v[0] for k, v in urllib.parse.parse_qs(raw).items()}
        for k, v in body.items():
            q.setdefault(k, [v if isinstance(v, bool) else str(v)])
        return q

    def _v1(self, u, q):
        g = lambda k: (q.get(k) or [""])[0]
        if u.path == "/api/v1/hello":
            code = str(g("code")).strip().upper()
            if not CODE_RE.fullmatch(code):
                return self._send(400, {"err": "工牌码是 6 位字母数字（工牌设置页下方能看到）"})
            item, err = hub.send_hello(code)
            return self._send(200 if item else 429, {"ok": True, "notice": item} if item else {"err": err})
        me = hub.code_of_token(str(g("token")))
        if not me:
            return self._send(403, {"err": "链接不对或已失效，请在工牌【设置 → 我的消息】重新扫码"})
        if u.path == "/api/v1/me":
            return self._send(200, {"code": me, "name": name_of(me), "inbox": hub.inbox(me), "sent": hub.sent_by(me),
                                    "people": people_list(exclude=me), "sub": hub.get_sub(me)})
        if u.path == "/api/v1/send":
            to = str(g("to")).strip().upper()
            if not CODE_RE.fullmatch(to):
                return self._send(400, {"err": "收件人不对"})
            people = {x["code"] for x in people_list()}
            if not name_of(me):
                return self._send(403, {"err": "先在工牌上扫码登记姓名，才能给同学发消息"})
            if to not in people:
                return self._send(400, {"err": "没找到这位同学"})
            item, err = hub.send_chat(me, name_of(me), to, str(g("text")))
            return self._send(200 if item else 429, {"ok": True, "notice": item} if item else {"err": err})
        if u.path == "/api/v1/remind":
            item, err = hub.send_mine(me, str(g("title")), str(g("text")))
            return self._send(200 if item else 429, {"ok": True, "notice": item, "tip": "工牌一两秒内会响一声"} if item else {"err": err})
        if u.path == "/api/v1/points":
            on = g("on")
            err = hub.set_sub(me, on is True or str(on).lower() in ("1", "true", "on", "yes"), str(g("time")) or "09:00")
            return self._send(400 if err else 200, {"err": err} if err else {"ok": True, "sub": hub.get_sub(me)})
        self._send(404, {"err": "not found"})

    def _admin(self, q):
        ak = CFG.get("admin_key") or ""
        if not ak or not hmac.compare_digest(str((q.get("key") or [""])[0]).encode(), ak.encode()):
            return self._html(403, "需要管理口令")
        names = {p["code"]: p["name"] for p in people_list()}
        esc = htmlmod.escape
        rows = []
        for i, c, k, t, x, sd, at, hid in hub.admin_list():
            btn = "" if hid else ("<form method=post action=/admin/hide><input type=hidden name=key value='%s'>"
                                  "<input type=hidden name=id value=%d><button>隐藏</button></form>" % (esc(ak), i))
            rows.append("<tr%s><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
                ' style="opacity:.4"' if hid else "", i, time.strftime("%m-%d %H:%M", time.localtime(at)), esc(k),
                esc(names.get(sd, sd)), esc(names.get(c, c)), esc(t), esc(x), btn))
        rows = "".join(rows)
        self._html(200, "<meta charset=utf-8><meta name=viewport content='width=device-width'><title>消息记录</title>"
                        "<style>body{font:14px system-ui;margin:16px}td{border-bottom:1px solid #ddd;padding:4px 6px}</style>"
                        "<h2>最近 200 条消息</h2><table><tr><th>#</th><th>时间</th><th>类型</th><th>谁发的</th><th>发给</th><th>标题</th><th>内容</th><th></th></tr>"
                        + rows + "</table>")

    def do_GET(self):
        u, q = parse_url(self.path)
        if u.path.startswith("/m/"):
            return self._html(200, open(os.path.join(HERE, "my.html"), encoding="utf-8").read())
        if u.path.startswith("/api/v1/"):
            return self._v1(u, q)
        if u.path == "/admin":
            return self._admin(q)
        if u.path == "/healthz":
            with _lock:
                return self._send(200, {"ok": True, "synced": int(_cache["at"]), "error": _cache["error"], "badges": len(_seen)})
        if u.path == "/api/push":
            return self._push(q)
        if not hmac.compare_digest(self.headers.get("X-Badge-Key", ""), CFG["badge_key"]):
            return self._send(403, {"err": "forbidden"})
        if u.path == "/api/wait":
            return self._wait(q)
        if u.path == "/api/badge":
            code = (q.get("code") or [""])[0].strip().upper()
            if not CODE_RE.fullmatch(code):
                return self._send(400, {"err": "bad code"})
            try:
                bat = int((q.get("bat") or ["-1"])[0])
            except ValueError:
                bat = -1
            note_badge(code, bat if 0 <= bat <= 100 else None, (q.get("ver") or [""])[0][:8])
            if (q.get("fresh") or ["0"])[0] == "1":
                refresh_fast()
            return self._send(200, build(code))
        self._send(404, {"err": "not found"})

    def _wait(self, q):
        """长连接：工牌带上自己知道的序号来等；序号变了（有新推送）马上返回，没有就等满 t 秒返回当前序号。"""
        code = (q.get("code") or [""])[0].strip().upper()
        if not CODE_RE.fullmatch(code):
            return self._send(400, {"err": "bad code"})
        try:
            since = int((q.get("seq") or ["-1"])[0])
            t = max(1, min(25, int((q.get("t") or ["12"])[0])))
        except ValueError:
            return self._send(400, {"err": "bad seq"})
        end = time.time() + t
        with _cv:
            while _seq.get(code, 0) == since and time.time() < end:
                _cv.wait(end - time.time())
            n = _seq.get(code, 0)
        self._send(200, {"seq": n})

    def _push(self, q):
        # 课堂推送：GET/POST /api/push?key=课堂口令&code=工牌码&title=标题&text=内容
        pk = CFG.get("push_key") or ""
        g = lambda k: (q.get(k) or [""])[0]
        if not pk:
            return self._send(404, {"err": "not found"})
        if not hmac.compare_digest(g("key").encode("utf-8"), pk.encode("utf-8")):
            return self._send(403, {"err": "口令不对"})
        code = g("code").strip().upper()
        if not CODE_RE.fullmatch(code):
            return self._send(400, {"err": "工牌码是 6 位（屏幕或背面能看到）"})
        if not g("title").strip():
            return self._send(400, {"err": "缺少标题 title"})
        item = push_notice(code, g("title"), g("text"))
        self._send(200, {"ok": True, "code": code, "notice": item, "tip": "工牌下次同步时显示在通知页"})

    def do_POST(self):
        u, q = parse_url(self.path)
        if u.path not in ("/api/push", "/api/v1/send", "/api/v1/remind", "/api/v1/points", "/admin/hide"):
            return self._send(404, {"err": "not found"})
        try:
            self._body(q)
        except Exception:
            return self._send(400, {"err": "bad body"})
        if u.path == "/admin/hide":
            ak = CFG.get("admin_key") or ""
            if ak and hmac.compare_digest(str((q.get("key") or [""])[0]).encode(), ak.encode()):
                hub.admin_hide(int((q.get("id") or ["0"])[0]))
                self.send_response(303); self.send_header("Location", "/admin?key=" + urllib.parse.quote(ak)); self.end_headers()
                return
            return self._html(403, "需要管理口令")
        if u.path.startswith("/api/v1/"):
            return self._v1(u, q)
        self._push(q)

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % a))


if __name__ == "__main__":
    hub.init(CFG.get("db") or os.path.join(HERE, "data.db"))
    hub.migrate_json(PUSH_FILE)
    threading.Thread(target=refresh_loop, daemon=True).start()
    threading.Thread(target=registry_loop, daemon=True).start()
    if CFG.get("discovery", True):
        threading.Thread(target=discovery_loop, daemon=True).start()
    if CFG.get("points_html"):
        threading.Thread(target=hub.points_loop, args=(CFG["points_html"],), daemon=True).start()
    host, port = CFG.get("bind", "127.0.0.1"), int(CFG.get("port", 8787))
    print(f"AI 工牌同步服务 http://{host}:{port}（局域网自动发现 UDP {DISC_PORT}）", flush=True)
    ThreadingHTTPServer((host, port), H).serve_forever()
