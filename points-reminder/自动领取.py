#!/usr/bin/env python3
"""电脑每天自动领 AI 积分（只用 Python 标准库 + agent-browser 浏览器工具）。

  python3 自动领取.py 检查                     看看环境齐不齐、哪些网站已登录
  python3 自动领取.py 登录 runninghub          弹出浏览器窗口，本人登录一次（魔搭写 modelscope）
  python3 自动领取.py 运行                     现在领一次（定时任务每天调用的就是它）
  python3 自动领取.py 定时 09:00               每天 09:00 自动运行（电脑开着才会运行）
  python3 自动领取.py 定时 关                  取消每天自动运行
  python3 自动领取.py 设置 --我的链接 <链接>    领完把结果发到自己的 AI 工牌（可选）

能自动领的：WorkBuddy（打开客户端即签到）、RunningHub（每日登录 30 积分）、魔搭社区（每日登录 200 魔粒）。
登录信息只存在本机 ~/.points-reminder/ 里（浏览器资料夹），不上传、不进安装包；不保存账号密码。
"""
import datetime, json, os, platform, re, shutil, subprocess, sys, time, urllib.request

HOME = os.path.join(os.path.expanduser("~"), ".points-reminder")
PROFILE = os.path.join(HOME, "browser-profile")
LOGDIR = os.path.join(HOME, "日志")
SETTINGS = os.path.join(HOME, "设置.json")
TODAY_FILE = os.path.join(HOME, "今天.json")
SESSION = "points-auto"
IS_MAC, IS_WIN = sys.platform == "darwin", sys.platform.startswith("win")

SITES = {
    "runninghub": {"名称": "RunningHub", "网址": "https://www.runninghub.cn/",
                   # 页面上出现这些字样（可见）就说明没登录
                   "未登录": ["登录领积分", "立即登录领取", "登录/注册", "登录 / 注册", "登录"]},
    "modelscope": {"名称": "魔搭社区", "网址": "https://www.modelscope.cn/",
                   "未登录": ["登录 / 注册", "登录/注册", "登录", "立即登录"]},
}
CLAIM_WORDS = ["领取", "立即领取", "领取积分", "一键领取", "领取奖励", "领取魔粒", "签到", "立即签到", "每日签到"]
DONE_WORDS = ["领取成功", "签到成功", "已领取", "已签到", "今日已领", "明日再来"]


# ---------------------------------------------------------------- 小工具
def load_settings():
    try:
        return json.load(open(SETTINGS, encoding="utf-8"))
    except Exception:
        return {}


def save_settings(s):
    os.makedirs(HOME, exist_ok=True)
    json.dump(s, open(SETTINGS, "w", encoding="utf-8"), ensure_ascii=False, indent=1)


def log(line):
    os.makedirs(LOGDIR, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%H:%M:%S")
    with open(os.path.join(LOGDIR, datetime.date.today().isoformat() + ".txt"), "a", encoding="utf-8") as f:
        f.write("%s %s\n" % (stamp, line))
    print(line, flush=True)


def clean_old_logs(days=14):
    if not os.path.isdir(LOGDIR):
        return
    cut = time.time() - days * 86400
    for n in os.listdir(LOGDIR):
        p = os.path.join(LOGDIR, n)
        if os.path.isfile(p) and os.path.getmtime(p) < cut:
            os.remove(p)


def find_agent_browser():
    s = load_settings()
    cands = [s.get("agent_browser"), shutil.which("agent-browser"),
             os.path.expanduser("~/npm-global/bin/agent-browser"), "/opt/homebrew/bin/agent-browser", "/usr/local/bin/agent-browser",
             os.path.join(os.environ.get("APPDATA", ""), "npm", "agent-browser.cmd"),
             # 用 WorkBuddy 自带的 npm 装的
             os.path.expanduser("~/.workbuddy/binaries/node/versions/current/bin/agent-browser"),
             os.path.expanduser("~/.workbuddy/binaries/node/versions/current/agent-browser.cmd")]
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


def find_browser():
    """用电脑上已有的 Chrome / Edge（不用另外下载浏览器；国内下载 agent-browser 自带的浏览器经常失败）。"""
    s = load_settings()
    pf, pf86, local = os.environ.get("ProgramFiles", ""), os.environ.get("ProgramFiles(x86)", ""), os.environ.get("LOCALAPPDATA", "")
    cands = [s.get("浏览器"),
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
             "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
             os.path.join(pf, "Google", "Chrome", "Application", "chrome.exe"),
             os.path.join(pf86, "Google", "Chrome", "Application", "chrome.exe"),
             os.path.join(local, "Google", "Chrome", "Application", "chrome.exe"),
             os.path.join(pf86, "Microsoft", "Edge", "Application", "msedge.exe"),
             os.path.join(pf, "Microsoft", "Edge", "Application", "msedge.exe")]
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


AB = None
BROWSER = None


def ab(*args, timeout=60, headed=False):
    """调用 agent-browser，返回 (成功?, 输出文字)。"""
    cmd = [AB, "--session", SESSION, "--profile", PROFILE] + (["--executable-path", BROWSER] if BROWSER else []) \
        + (["--headed"] if headed else []) + list(args)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([os.path.dirname(AB), os.environ.get("PATH", "")])
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env,
                           encoding="utf-8", errors="replace")
        return r.returncode == 0, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        return False, "超时"


def ab_eval(js, timeout=30):
    ok, out = ab("eval", js, timeout=timeout)
    if not ok:
        return None
    out = out.strip()
    for _ in range(2):  # agent-browser 会把字符串结果再包一层引号
        try:
            out = json.loads(out)
        except Exception:
            break
    return out


def js_state(words):
    """在页面里判断：有没有可见的“登录”入口；页面上有没有“已领取”之类的字样。"""
    return r"""JSON.stringify((function(){
  var W=%s, D=%s, vis=function(e){var r=e.getBoundingClientRect();var s=getComputedStyle(e);return r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none';};
  var els=[].slice.call(document.querySelectorAll('a,button,span,div,[role=button]'));
  var login=els.filter(function(e){var t=(e.innerText||'').trim();return W.indexOf(t)>=0&&vis(e)&&(t.length>2||e.getBoundingClientRect().top<160);}).map(function(e){return (e.innerText||'').trim();});
  var body=document.body?document.body.innerText:'';
  return {login:login.slice(0,3), done:D.filter(function(w){return body.indexOf(w)>=0;}), url:location.href, title:document.title};
})())""" % (json.dumps(words, ensure_ascii=False), json.dumps(DONE_WORDS, ensure_ascii=False))


# 只点“积分 / 魔粒 / 签到”旁边的领取按钮；周围出现会员、购买、支付、价格字样的一律不点（避免误点促销）。
JS_CLAIM = r"""JSON.stringify((function(){
  var W=%s, OK=/积分|魔粒|签到|RH币|每日|今日/, NO=/会员|购买|支付|充值|开通|订阅|优惠券|折|¥|￥|元\//;
  var vis=function(e){var r=e.getBoundingClientRect();var s=getComputedStyle(e);return r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none'&&!e.disabled;};
  // 离按钮最近、有别的文字的那一层（卡片 / 弹窗），只看它
  var ctx=function(e){var t=(e.innerText||'').trim(),p=e;for(var i=0;i<5&&p.parentElement;i++){p=p.parentElement;if((p.innerText||'').trim().length>t.length+3)break;}return (p.innerText||'').slice(0,300);};
  var hit=[].slice.call(document.querySelectorAll('button,a,[role=button],div,span')).filter(function(e){
    var t=(e.innerText||'').trim(); if(W.indexOf(t)<0||!vis(e)||e.children.length>2)return false;
    var c=ctx(e); return OK.test(c)&&!NO.test(c);});
  var clicked=[]; hit.slice(0,2).forEach(function(e){try{e.click();clicked.push((e.innerText||'').trim());}catch(x){}});
  return clicked;
})())""" % json.dumps(CLAIM_WORDS, ensure_ascii=False)


# ---------------------------------------------------------------- 各平台
def open_workbuddy():
    if IS_MAC:
        app = "/Applications/WorkBuddy.app"
        if not os.path.exists(app):
            return "没装", "这台电脑上没找到 WorkBuddy"
        subprocess.run(["open", "-g", "-a", app])
        return "已领", "已打开客户端（打开即签到）"
    if IS_WIN:
        for p in [os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "WorkBuddy", "WorkBuddy.exe"),
                  os.path.join(os.environ.get("ProgramFiles", ""), "WorkBuddy", "WorkBuddy.exe")]:
            if os.path.exists(p):
                os.startfile(p)
                return "已领", "已打开客户端（打开即签到）"
        return "没装", "没找到 WorkBuddy.exe，请确认安装位置"
    return "跳过", "这个系统不支持"


def visit(key):
    """打开网站 → 判断登录 → 有“领取/签到”按钮就点 → 截图留证。返回 (状态, 说明)。"""
    site = SITES[key]
    ok, out = ab("open", site["网址"], timeout=90)
    if not ok:
        return "失败", "打不开网站：" + out.strip()[-80:]
    ab("wait", "4000", timeout=30)
    st = ab_eval(js_state(site["未登录"])) or {}
    if st.get("login"):
        return "要登录", "登录过期，请运行：自动领取.py 登录 " + key
    clicked = []
    for _ in range(2):  # 有的网站点完会再弹一个确认框
        c = ab_eval(JS_CLAIM) or []
        if not c:
            break
        clicked += c
        ab("wait", "2500", timeout=30)
    st2 = ab_eval(js_state(site["未登录"])) or {}
    shot = os.path.join(LOGDIR, "%s-%s.png" % (datetime.date.today().isoformat(), key))
    ab("screenshot", shot, timeout=30)
    if clicked:
        return "已领", "已点“%s”" % "、".join(dict.fromkeys(clicked))
    if st2.get("done"):
        return "已领", "页面显示“%s”" % st2["done"][0]
    return "已领", "已登录访问（每日登录即发放，以账户为准）"


# ---------------------------------------------------------------- 命令
def cmd_check():
    print("系统：", platform.system(), platform.release())
    print("Python：", sys.version.split()[0])
    print("agent-browser：", AB or "没找到（安装：npm i -g agent-browser --registry=https://registry.npmmirror.com）")
    print("浏览器：", BROWSER or "没找到 Chrome / Edge，将使用 agent-browser 自带浏览器（需先运行 agent-browser install）")
    if IS_MAC:
        print("WorkBuddy：", "已安装" if os.path.exists("/Applications/WorkBuddy.app") else "没找到")
    s = load_settings()
    print("登录资料夹：", PROFILE if os.path.isdir(PROFILE) else "还没有（先运行：登录 runninghub）")
    print("已登录过：", "、".join(SITES[k]["名称"] for k in s.get("已登录", [])) or "无")
    print("每天定时：", s.get("定时") or "未设置")
    print("工牌推送：", "已设置" if s.get("我的链接") else "未设置（可选）")
    if os.path.exists(TODAY_FILE):
        print("上次运行：", open(TODAY_FILE, encoding="utf-8").read()[:300])


def cmd_login(key):
    if key not in SITES:
        sys.exit("网站只能是：" + " / ".join(SITES))
    if not AB:
        sys.exit("没找到 agent-browser，先安装：npm i -g agent-browser && agent-browser install")
    site = SITES[key]
    ab("close", timeout=20)  # 关掉后台那个无头浏览器，才能用窗口模式重新打开
    ok, out = ab("open", site["网址"], timeout=90, headed=True)
    if not ok:
        sys.exit("浏览器没打开：" + out[-200:])
    print("已打开 %s，请在弹出的浏览器窗口里登录（扫码或验证码）。登录好后不用关窗口，这里会自动检测，最多等 5 分钟……" % site["名称"], flush=True)
    t0 = time.time()
    while time.time() - t0 < 300:
        time.sleep(4)
        st = ab_eval(js_state(site["未登录"]))
        if isinstance(st, dict) and not st.get("login") and "login" not in (st.get("url") or "").lower():
            s = load_settings()
            s["已登录"] = sorted(set(s.get("已登录", []) + [key]))
            save_settings(s)
            ab("wait", "2000")
            ab("close", timeout=20)
            print("登录成功，%s 以后每天会自动领取。" % site["名称"])
            return
    ab("close", timeout=20)
    sys.exit("5 分钟内没检测到登录。可以再运行一次：自动领取.py 登录 " + key)


def notify_badge(title, text):
    link = load_settings().get("我的链接", "")
    m = re.match(r"(https?://[^/]+)/m/([A-Za-z0-9_-]+)", link.strip())
    if not m:
        return None
    body = json.dumps({"token": m.group(2), "title": title[:16], "text": text[:40]}).encode("utf-8")
    req = urllib.request.Request(m.group(1) + "/api/v1/remind", body, {"Content-Type": "application/json"})
    try:
        r = json.loads(urllib.request.urlopen(req, timeout=10).read().decode("utf-8"))
        return "已发到工牌" if r.get("ok") else "工牌没发成功：%s" % r.get("err")
    except Exception as e:
        return "工牌没发成功：%s" % e


def cmd_run():
    clean_old_logs()
    s = load_settings()
    log("== 开始自动领取")
    res = [("WorkBuddy",) + open_workbuddy()]
    sites = [k for k in SITES if k in s.get("已登录", [])]
    if sites and not AB:
        res += [(SITES[k]["名称"], "失败", "没找到 agent-browser") for k in sites]
        sites = []
    for k in sites:
        try:
            res.append((SITES[k]["名称"],) + visit(k))
        except Exception as e:
            res.append((SITES[k]["名称"], "失败", str(e)[:80]))
    if sites:
        ab("close", timeout=20)
    for name, state, note in res:
        log("%s：%s（%s）" % (name, state, note))
    got = [n for n, st, _ in res if st == "已领"]
    need = [n for n, st, _ in res if st == "要登录"]
    bad = [n for n, st, _ in res if st == "失败"]
    title = "自动领了 %d 项积分" % len(got)
    text = "、".join(got) if got else "今天没领到"
    if need:
        text += "；%s要重新登录" % "、".join(need)
    if bad:
        text += "；%s失败" % "、".join(bad)
    os.makedirs(HOME, exist_ok=True)
    json.dump({"日期": datetime.date.today().isoformat(), "时间": datetime.datetime.now().strftime("%H:%M"),
               "结果": [{"平台": n, "状态": st, "说明": note} for n, st, note in res], "摘要": title + "：" + text},
              open(TODAY_FILE, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    r = notify_badge(title, text)
    if r:
        log(r)
    log("摘要：" + title + "：" + text)


def cmd_due():
    """定时任务每 10 分钟调一次：过了设定时间、今天还没领过，才真正去领；否则什么都不做。
    这样不怕时区、睡眠、关机：电脑 9 点没开，开机后第一次检查就会补领。"""
    s = load_settings()
    hhmm = s.get("定时")
    if not hhmm:
        return
    now = datetime.datetime.now()
    try:
        last = json.load(open(TODAY_FILE, encoding="utf-8")).get("日期")
    except Exception:
        last = None
    if last == now.date().isoformat() or now.strftime("%H:%M") < hhmm:
        return
    cmd_run()


def cmd_schedule(arg):
    s = load_settings()
    me = os.path.join(HOME, "自动领取.py")  # 复制一份到 ~/.points-reminder，避开“桌面/文稿”的系统权限限制
    if arg in ("关", "off"):
        if IS_MAC:
            plist = os.path.expanduser("~/Library/LaunchAgents/com.points-reminder.autoclaim.plist")
            subprocess.run(["launchctl", "unload", "-w", plist], capture_output=True)
            if os.path.exists(plist):
                os.remove(plist)
        elif IS_WIN:
            subprocess.run(["schtasks", "/Delete", "/TN", "积分自动领取", "/F"], capture_output=True)
        s.pop("定时", None)
        save_settings(s)
        print("已取消每天自动领取。")
        return
    if not re.fullmatch(r"([01]?\d|2[0-3]):[0-5]\d", arg or ""):
        sys.exit("时间写成 09:00 这样")
    hh, mm = [int(x) for x in arg.split(":")]
    os.makedirs(HOME, exist_ok=True)
    shutil.copyfile(os.path.abspath(__file__), me)
    if AB:
        s["agent_browser"] = AB
    if BROWSER:
        s["浏览器"] = BROWSER
    if IS_MAC:
        plist = os.path.expanduser("~/Library/LaunchAgents/com.points-reminder.autoclaim.plist")
        path = os.pathsep.join(filter(None, [os.path.dirname(AB) if AB else "", os.path.dirname(sys.executable),
                                             "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"]))
        xml = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
 <key>Label</key><string>com.points-reminder.autoclaim</string>
 <key>ProgramArguments</key><array><string>%s</string><string>%s</string><string>到点就领</string></array>
 <key>StartInterval</key><integer>600</integer>
 <key>RunAtLoad</key><true/>
 <key>EnvironmentVariables</key><dict><key>PATH</key><string>%s</string></dict>
 <key>StandardOutPath</key><string>%s</string><key>StandardErrorPath</key><string>%s</string>
</dict></plist>
""" % (sys.executable, me, path, os.path.join(HOME, "定时输出.txt"), os.path.join(HOME, "定时输出.txt"))
        os.makedirs(os.path.dirname(plist), exist_ok=True)
        subprocess.run(["launchctl", "unload", "-w", plist], capture_output=True)
        open(plist, "w", encoding="utf-8").write(xml)
        r = subprocess.run(["launchctl", "load", "-w", plist], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("定时没设置成功：" + (r.stderr or r.stdout))
    elif IS_WIN:
        tr = '"%s" "%s" 到点就领' % (sys.executable.replace("python.exe", "pythonw.exe"), me)
        r = subprocess.run(["schtasks", "/Create", "/TN", "积分自动领取", "/SC", "MINUTE", "/MO", "10", "/TR", tr, "/F"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("定时没设置成功：" + (r.stderr or r.stdout))
    else:
        sys.exit("这个系统请用 crontab 设置")
    s["定时"] = "%02d:%02d" % (hh, mm)
    save_settings(s)
    print("好了，每天 %02d:%02d 以后自动领一次（电脑那时没开的，开机后会补领）。" % (hh, mm))


def cmd_set(args):
    s = load_settings()
    if "--我的链接" in args:
        link = args[args.index("--我的链接") + 1]
        if not re.match(r"https?://[^/]+/m/[A-Za-z0-9_-]+", link):
            sys.exit("“我的链接”形如 http://118.24.193.151/m/xxxxxxxxxx（工牌：设置 → 我的消息 → 扫码打开的网页地址）")
        s["我的链接"] = link.strip()
    save_settings(s)
    print("已保存。")


if __name__ == "__main__":
    AB = find_agent_browser()
    BROWSER = find_browser()
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help", "帮助"):
        print(__doc__)
    elif a[0] == "检查":
        cmd_check()
    elif a[0] == "登录" and len(a) > 1:
        cmd_login(a[1].lower())
    elif a[0] == "运行":
        cmd_run()
    elif a[0] == "到点就领":
        cmd_due()
    elif a[0] == "定时" and len(a) > 1:
        cmd_schedule(a[1])
    elif a[0] == "设置":
        cmd_set(a[1:])
    else:
        print(__doc__)
