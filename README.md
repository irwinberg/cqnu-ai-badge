# 重师大 AI 工牌 · cqnu-ai-badge

把 FoloToy AI Passport（ESP32-C3 小屏工牌）做成培训用的“AI 工牌”：上课提醒、扫码交作业、看分数、参加比赛（积分、排名、投票），还能接收老师、同学和自己设置的消息。老师在飞书多维表格里维护课表、作业、通知和分数，工牌自动同步。

| 目录 | 内容 |
|---|---|
| [`firmware/`](firmware/) | 工牌固件（基于 FoloToy 开源的 [ai-passport](https://gitee.com/FoloToy/ai-passport)，ESP-IDF 5.5 + LVGL 9） |
| [`server/`](server/) | 同步服务和消息中心：读飞书多维表格，给每张工牌返回它要显示的内容；收发消息；“我的消息”网页 |
| [`flash-kit/`](flash-kit/) | WorkBuddy 刷机技能：一步一步带学员刷机、连 Wi-Fi、扫码登记 |

积分提醒站是单独的仓库：[points-reminder](https://github.com/irwinberg/points-reminder)（它要能直接装成 WorkBuddy 技能，所以不放在这里）。

## 学员：刷机

到 [Releases](../../releases) 下载对应系统的 `cqnu-badge-mac.zip` 或 `cqnu-badge-windows.zip`，解压后放进 WorkBuddy 的技能目录（`~/.workbuddy/skills/`），然后对 WorkBuddy 说“帮我配置 AI 工牌”。详细步骤见 [`flash-kit/先看我.md`](flash-kit/先看我.md)。

想自己加功能：只改 [`firmware/main/my_feature.c`](firmware/main/my_feature.c)，说明在同目录的 `my_feature.h`。

## 老师：日常维护（全部在飞书多维表格里）

| 想做的事 | 在哪张表 |
|---|---|
| 维护老师、学员名单 | 【人员】：身份选“教师”或“学生”，填工牌码。学员也可以在工牌上扫码自助登记（写进【工牌登记】） |
| 排课、通知上课时间 | 【课表】：时间、地点、上课班级，授课老师关联到【人员】。开课前 10 分钟工牌响铃 |
| 布置作业 | 【作业】：勾“已发布”。学员在工牌上扫码交 |
| 打分、写评语 | 【作业提交】：填“分数”“评语”，学员工牌马上显示 |
| 发通知 | 【通知】：选发送对象，勾“已发布”，工牌整页弹出 |
| 比赛 | 【作品】【评委打分】【观众投票】 |

## 工作原理

```
飞书多维表格 ──(每 30 秒读一次)──> server/（云服务器）
                                      │  /api/badge  每张工牌的数据（课表、作业、分数、通知）
                                      │  /api/wait   长连接，新消息 1–2 秒内到达
                                      │  /m/<个人密钥> 我的消息：收件箱、给同学发消息、每天积分提醒
                                      ▼
                                  每张工牌（Wi-Fi，2.4G）
```

消息分三类，互不挤占：**课堂**（老师用课堂口令推送、【通知】表）、**同学**（“我的消息”网页里互发，需已登记、限频）、**提醒**（自己的程序或积分站发给自己，`POST /api/v1/remind`）。

## 自己部署后台

1. 服务器装 Python 3、nginx、Node 22，`npm i -g @larksuite/cli`，`lark-cli config init` 配好一个飞书应用，并把这个应用加为多维表格的“可编辑”协作者。
2. 复制 `server/` 到 `/opt/cqnu/server/`，按 `config.example.json` 写 `config.json`。
3. 按 `server/deploy/` 里的 systemd 和 nginx 配置启动。
4. 固件里把 `CONFIG_CQ_SERVER_URL` 设成你的服务器地址（写在 `firmware/sdkconfig.secrets`，同时确认 `firmware/sdkconfig` 里也是这个值），重新编译。

密钥（工牌访问密钥、课堂口令、管理口令、飞书凭证）都只放在服务器的 `config.json` 和本机的 `sdkconfig.secrets` 里，不进仓库。

## 许可

固件部分基于 FoloToy ai-passport（MIT License，见 [`firmware/LICENSE`](firmware/LICENSE)），本仓库新增部分同样采用 MIT License。
