// app_model.c —— 把同步电脑返回的 JSON 解析成 cq_model_t
#include "app.h"
#include "cJSON.h"
#include <string.h>

static void str(cJSON *o, const char *k, char *dst, size_t len)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v) || !v->valuestring || len == 0) {
        if (len) dst[0] = '\0';
        return;
    }
    const char *s = v->valuestring;
    size_t n = strlen(s);
    if (n >= len) {
        n = len - 1;
        // s[n] 是第一个放不下的字节；若它是 UTF-8 续字节，退回到该字符的首字节，整字丢弃
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static int64_t i64(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

static bool bl(cJSON *o, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

static int classes(cJSON *arr, cq_class_t *dst)
{
    int n = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, arr) {
        if (n >= CQ_MAX_CLASSES) break;
        dst[n].t = i64(c, "t");
        dst[n].end = i64(c, "end");
        str(c, "name", dst[n].name, sizeof dst[n].name);
        str(c, "place", dst[n].place, sizeof dst[n].place);
        str(c, "tip", dst[n].tip, sizeof dst[n].tip);
        n++;
    }
    return n;
}

bool cq_model_parse(const char *json, cq_model_t *m)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    memset(m, 0, sizeof *m);
    m->now = i64(root, "now");
    m->bound = bl(root, "bound");
    str(root, "code", m->code, sizeof m->code);
    str(root, "name", m->name, sizeof m->name);
    str(root, "org", m->org, sizeof m->org);
    str(root, "sid", m->sid, sizeof m->sid);
    m->avatar = (int)i64(root, "avatar");
    m->modes = APP_MODE_BIT(APP_MODE_VIS);
    cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(root, "modes")) {
        if (!cJSON_IsString(v)) continue;
        if (!strcmp(v->valuestring, "stu")) m->modes |= APP_MODE_BIT(APP_MODE_STU);
        else if (!strcmp(v->valuestring, "tea")) m->modes |= APP_MODE_BIT(APP_MODE_TEA);
        else if (!strcmp(v->valuestring, "con")) m->modes |= APP_MODE_BIT(APP_MODE_CON);
    }
    cJSON *tpl = cJSON_GetObjectItemCaseSensitive(root, "tpl");
    if (cJSON_IsObject(tpl)) {
        str(tpl, "hw", m->tpl_hw, sizeof m->tpl_hw);
        str(tpl, "vote", m->tpl_vote, sizeof m->tpl_vote);
    }

    cJSON *stu = cJSON_GetObjectItemCaseSensitive(root, "stu");
    if (cJSON_IsObject(stu)) {
        m->n_stu_classes = classes(cJSON_GetObjectItemCaseSensitive(stu, "classes"), m->stu_classes);
        cJSON *h;
        cJSON_ArrayForEach(h, cJSON_GetObjectItemCaseSensitive(stu, "hw")) {
            if (m->n_hw >= CQ_MAX_HW) break;
            cq_hw_t *d = &m->hw[m->n_hw++];
            str(h, "id", d->id, sizeof d->id);
            str(h, "name", d->name, sizeof d->name);
            d->due = i64(h, "due");
            d->st = (int)i64(h, "st");
            d->has_score = cJSON_GetObjectItemCaseSensitive(h, "score") != NULL;
            d->score = (int)i64(h, "score");
            str(h, "note", d->note, sizeof d->note);
        }
        m->stu_score = (int)i64(stu, "score");
        m->stu_done = (int)i64(stu, "done");
        m->stu_total = (int)i64(stu, "total");
    }

    cJSON *tea = cJSON_GetObjectItemCaseSensitive(root, "tea");
    if (cJSON_IsObject(tea)) {
        m->n_tea_classes = classes(cJSON_GetObjectItemCaseSensitive(tea, "classes"), m->tea_classes);
        cJSON *hw = cJSON_GetObjectItemCaseSensitive(tea, "hw");
        if (cJSON_IsObject(hw)) {
            m->tea_has_hw = true;
            str(hw, "name", m->tea_hw_name, sizeof m->tea_hw_name);
            m->tea_sub = (int)i64(hw, "sub");
            m->tea_total = (int)i64(hw, "total");
            m->tea_graded = (int)i64(hw, "graded");
            m->tea_todo = (int)i64(hw, "todo");
            cJSON *n;
            cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(hw, "missing")) {
                if (!cJSON_IsString(n)) continue;
                size_t cur = strlen(m->tea_missing);
                if (cur + strlen(n->valuestring) + 4 >= sizeof m->tea_missing) break;
                if (cur) strcat(m->tea_missing, "、");
                strcat(m->tea_missing, n->valuestring);
            }
        }
    }

    cJSON *con = cJSON_GetObjectItemCaseSensitive(root, "con");
    if (cJSON_IsObject(con)) {
        str(con, "work", m->con_work, sizeof m->con_work);
        str(con, "no", m->con_no, sizeof m->con_no);
        m->con_score = (int)i64(con, "score");
        m->con_rank = (int)i64(con, "rank");
        m->con_of = (int)i64(con, "of");
        m->con_gap = (int)i64(con, "gap");
        m->con_votes = (int)i64(con, "votes");
        cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(con, "top")) {
            if (m->n_top >= CQ_MAX_TOP) break;
            cq_top_t *d = &m->top[m->n_top++];
            d->r = (int)i64(t, "r");
            d->s = (int)i64(t, "s");
            d->av = (int)i64(t, "av");
            d->me = bl(t, "me");
            str(t, "n", d->n, sizeof d->n);
        }
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(con, "recent")) {
            if (m->n_recent >= CQ_MAX_RECENT) break;
            cq_score_t *d = &m->recent[m->n_recent++];
            d->s = (int)i64(t, "s");
            d->t = i64(t, "t");
            str(t, "d", d->d, sizeof d->d);
        }
        str(con, "judge_url", m->judge_url, sizeof m->judge_url);
    }

    cJSON *vis = cJSON_GetObjectItemCaseSensitive(root, "vis");
    if (cJSON_IsObject(vis)) {
        cJSON *w;
        cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(vis, "works")) {
            if (m->n_works >= CQ_MAX_WORKS) break;
            cq_work_t *d = &m->works[m->n_works++];
            str(w, "id", d->id, sizeof d->id);
            str(w, "no", d->no, sizeof d->no);
            str(w, "name", d->name, sizeof d->name);
            str(w, "intro", d->intro, sizeof d->intro);
            d->votes = (int)i64(w, "votes");
            d->voted = bl(w, "voted");
        }
        m->voted = (int)i64(vis, "voted");
    }

    cJSON *n;
    cJSON_ArrayForEach(n, cJSON_GetObjectItemCaseSensitive(root, "notices")) {
        if (m->n_notices >= CQ_MAX_NOTICES) break;
        cq_notice_t *d = &m->notices[m->n_notices++];
        str(n, "title", d->title, sizeof d->title);
        str(n, "text", d->text, sizeof d->text);
        d->at = i64(n, "at");
        str(n, "k", d->k, sizeof d->k);
    }

    str(root, "self_url", m->self_url, sizeof m->self_url);
    str(root, "my_url", m->my_url, sizeof m->my_url);
    str(root, "reg_url", m->reg_url, sizeof m->reg_url);
    cJSON *reg = cJSON_GetObjectItemCaseSensitive(root, "reg");
    if (cJSON_IsObject(reg)) {
        m->reg_exists = true;
        str(reg, "work", m->reg_work, sizeof m->reg_work);
        str(reg, "state", m->reg_state, sizeof m->reg_state);
    }

    cJSON *wl = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsArray(wl)) {
        m->has_wifi = true;
        cJSON *w;
        cJSON_ArrayForEach(w, wl) {
            if (m->n_wifi >= CQ_MAX_WIFI_PUSH) break;
            cq_wifi_t *d = &m->wifi[m->n_wifi];
            str(w, "s", d->ssid, sizeof d->ssid);
            str(w, "p", d->pass, sizeof d->pass);
            d->src = WIFI_SRC_SERVER;
            if (d->ssid[0]) m->n_wifi++;
        }
    }
    m->valid = true;
    cJSON_Delete(root);
    return true;
}

uint32_t cq_model_hash(const cq_model_t *m)
{
    // 结构体在解析前整体清零，填充字节也是 0；只跳过 now（每次都变）
    const uint8_t *p = (const uint8_t *)m;
    size_t skip_from = offsetof(cq_model_t, now), skip_to = skip_from + sizeof m->now;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *m; i++) {
        if (i >= skip_from && i < skip_to) continue;
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
