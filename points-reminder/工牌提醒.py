#!/usr/bin/env python3
"""把“今天要领的积分”推送到自己的 AI 工牌（只用 Python 标准库）。

用法：
  python3 工牌提醒.py --试一下                                   # 只打印要推送的内容，不发
  python3 工牌提醒.py --我的链接 http://118.24.193.151/m/你的个人密钥   # 发到自己的工牌
  python3 工牌提醒.py --网页 我的积分提醒站/index.html --我的链接 ...    # 默认读 模板/index.html

“我的链接”在工牌上找：设置 → 我的消息 → 扫码，打开的网页地址就是。它只能给你自己的工牌发提醒，
不要发给别人，也不要写进网页里。也可以放在环境变量 BADGE_MY_URL 里。
每天定时运行：交给 WorkBuddy 定时任务，例如“每天 9:00 运行这条命令”。
（不想开电脑？在“我的消息”网页里打开“每天积分提醒”，服务器会每天替你推送。）
"""
import argparse, datetime, json, os, re, sys, urllib.parse, urllib.request

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('--网页', default=os.path.join(here, '模板', 'index.html'))
ap.add_argument('--我的链接', default=os.environ.get('BADGE_MY_URL', ''))
ap.add_argument('--试一下', action='store_true')
a = ap.parse_args()

html = open(a.网页, encoding='utf-8').read()
block = lambda i: json.loads(re.search(r'<script type="application/json" id="%s">(.*?)</script>' % i, html, re.S).group(1))
cfg, deals = block('config'), block('deals')
today = datetime.date.today().isoformat()
show = cfg.get('显示的工具') or []
daily = [d for d in deals if d['周期'] == '每天' and d['工具'] in show and not (d.get('截止') and d['截止'] < today)]
daily.sort(key=lambda d: show.index(d['工具']))
ending = [d for d in deals if d.get('截止') and d['工具'] in show and 0 <= (datetime.date.fromisoformat(d['截止']) - datetime.date.today()).days <= 3]

title = ('今天领积分 %d 项' % len(daily))[:16]
text = '、'.join(dict.fromkeys(d['工具'] for d in daily))
if ending:
    text += '；%s快截止' % ending[0]['标题'][:10]
text = text[:40]
print('标题：', title)
print('内容：', text)
if a.试一下:
    sys.exit(0)

m = re.match(r'(https?://[^/]+)/m/([A-Za-z0-9_-]+)', a.我的链接.strip())
if not m:
    sys.exit('缺少 --我的链接（工牌上：设置 → 我的消息 → 扫码打开的网页地址，形如 http://118.24.193.151/m/xxxxxxxxxx）')
base, token = m.group(1), m.group(2)
body = json.dumps({'token': token, 'title': title, 'text': text}).encode('utf-8')
req = urllib.request.Request(base + '/api/v1/remind', body, {'Content-Type': 'application/json'})
try:
    r = json.loads(urllib.request.urlopen(req, timeout=10).read().decode('utf-8'))
except urllib.error.HTTPError as e:
    r = json.loads(e.read().decode('utf-8') or '{}')
except Exception as e:
    sys.exit('连不上服务器：%s' % e)
print('已推送，工牌一两秒内会响一声' if r.get('ok') else '没推送成功：%s' % r.get('err', r))
