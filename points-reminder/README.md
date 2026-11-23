# 积分提醒站 · points-reminder

每天提醒你领 AI 工具的免费积分，电脑开着还能自动领。这是一个 WorkBuddy 技能，装好后用说话的方式操作。

覆盖的工具：**WorkBuddy**（每天签到）、**RunningHub**（每日登录 30 积分）、**魔搭社区**（每日 200 魔粒，可调 DeepSeek）、**豆包**（新用户体验）、**DeepSeek**（网页版免费）。

- 在线看：<http://118.24.193.151/points/>
- 国内下载（推荐）：<https://gitee.com/fulingsimin/cqnu-ai-badge/releases>（附件 `points-reminder.zip`）
- GitHub：<https://github.com/irwinberg/cqnu-ai-badge/releases>（附件 `points-reminder.zip`），源码在仓库的 `points-reminder/` 目录

## 安装（交给 WorkBuddy）

打开 WorkBuddy，把下面这段话原样发给它：

> 请帮我安装“积分提醒站”技能：下载 https://gitee.com/fulingsimin/cqnu-ai-badge/releases/download/v4/points-reminder.zip（打不开就用 https://github.com/irwinberg/cqnu-ai-badge/releases/download/v4/points-reminder.zip），解压后把里面的 points-reminder 文件夹放进 WorkBuddy 的技能文件夹（Mac：`~/.workbuddy/skills/points-reminder`；Windows：`%USERPROFILE%\.workbuddy\skills\points-reminder`）。装好后告诉我。

装好后，对 WorkBuddy 说这几句就能用：

| 想做什么 | 对 WorkBuddy 说 |
|---|---|
| 做一个自己的积分提醒网页 | 帮我做一个积分提醒站 |
| 每天提醒我领 | 提醒我每天领积分 |
| 电脑每天自动领 | 帮我设置积分自动领取 |
| 活动有变化 | 更新一下积分活动 |

## 文件说明

| 文件 | 作用 |
|---|---|
| `SKILL.md` | 给 WorkBuddy 看的操作说明 |
| `模板/index.html` | 积分提醒网页（单文件，手机、电脑都能打开） |
| `自动领取.py` | 电脑每天自动领：WorkBuddy 签到、RunningHub、魔搭社区 |
| `工牌提醒.py` | 把“今天要领的”推送到自己的 AI 工牌 |
| `打包.py` | 维护者用：打学员安装包 |

## 隐私与安全

- 自动领取用你自己电脑上的浏览器，**第一次由你本人登录**；登录信息只存在你电脑的 `~/.points-reminder/` 里，不上传、不在这个仓库里。
- 不保存任何账号密码，不调用平台的私有接口。
- 只点签到 / 领积分的按钮，不点会员、购买、支付相关的按钮。
- 只收各平台官方公开的活动，不放邀请和返利链接。额度和规则以官网为准。
