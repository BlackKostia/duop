/* src/ui/progress_dialog.c - ProgressDialog (Inherits from Dialog): live view of the background worker
 * with current item, progress bar, byte/file counts, speed/ETA and [Pause|Resume] [Background] [Cancel]
 * buttons (freestanding C23; replaces progress_dialog{,_render,_input}.s, kept as tests/harness/progress_dialog*_reference.s). */
#include "duop_app.h"
#include "duop_classes.h"
#include "duop_keys.h"
#include "duop_string.h"
#include "duop_task.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "i18n_slots.h"

#define PD_W_MAX 66
#define PD_W_MIN 44
#define PD_H 9
#define PD_BTNS 3
#define PD_BUF 192

static void ProgressDialog_render(Widget *self, void *screen, void *theme);
static int ProgressDialog_on_key(Widget *self, void *event);
static int ProgressDialog_on_mouse(Widget *self, void *event);
static void ProgressDialog_on_hide(Dialog *self);

DIALOG_VTBL(ProgressDialog_vtbl, "ProgressDialog", Dialog_vtbl, sizeof(ProgressDialog),
            .base.layout = ProgressDialog_layout, .base.render = ProgressDialog_render,
            .base.on_key = ProgressDialog_on_key, .base.on_mouse = ProgressDialog_on_mouse,
            .on_confirm = ProgressDialog_on_hide, .on_cancel = ProgressDialog_on_hide);

static void pd_cat(char *buf, const char *s)
{
    (void)str_cat(buf, s, PD_BUF);
}

static void pd_cat_u64(char *buf, uint64_t v)
{
    char tmp[32];
    (void)u64_to_dec(v, tmp);
    pd_cat(buf, tmp);
}

static void pd_cat_size(char *buf, int64_t bytes)
{
    char tmp[32];
    (void)format_size(bytes, tmp);
    pd_cat(buf, tmp);
}

static void pd_fmt_eta(int64_t sec, char *buf)
{
    if (sec < 0) {
        (void)str_copy(buf, "--:--", 8);
        return;
    }
    uint64_t u = (uint64_t)sec;
    uint64_t sec_r = u % 60u;
    u /= 60u;
    uint64_t m = u % 60u;
    uint64_t h = u / 60u;
    char *p = buf;
    if (h) {
        p += u64_to_dec(h, p);
        *p++ = ':';
        if (m < 10) *p++ = '0';
    }
    p += u64_to_dec(m, p);
    *p++ = ':';
    if (sec_r < 10) *p++ = '0';
    (void)u64_to_dec(sec_r, p);
}

const char *ProgressDialog_title_for(int op)
{
    const LangStrings *ls = i18n_get();
    if (op == TASK_OP_MOVE) return ls->s[LS_task_title_move];
    if (op == TASK_OP_DELETE) return ls->s[LS_task_title_delete];
    if (op == TASK_OP_UNPACK) return ls->s[LS_task_title_unpack];
    if (op == TASK_OP_ARCTEST) return ls->s[LS_task_title_arctest];
    if (op == TASK_OP_PACK) return ls->s[LS_task_title_pack];
    return ls->s[LS_task_title_copy];
}

static void pd_format_state(const TaskSnap *s, char *buf)
{
    const LangStrings *ls = i18n_get();
    if (s->canceling) {
        pd_cat(buf, ls->s[LS_task_lbl_canceling]);
        return;
    }
    if (s->paused) {
        pd_cat(buf, ls->s[LS_task_lbl_paused]);
        return;
    }
    if (s->phase == TASK_PHASE_SCAN) {
        pd_cat(buf, ls->s[LS_task_lbl_scan]);
        pd_cat_u64(buf, s->total_files);
        return;
    }
    if (s->op != TASK_OP_DELETE) {
        pd_cat_size(buf, (int64_t)s->speed_bps);
        pd_cat(buf, "/s");
        pd_cat(buf, "  ·  ");
    }
    pd_cat(buf, ls->s[LS_task_lbl_eta]);
    char eta[32];
    pd_fmt_eta((int64_t)s->eta_sec, eta);
    pd_cat(buf, eta);
}

void ProgressDialog_format_summary(const void *snap, char *buf)
{
    const TaskSnap *s = (const TaskSnap *)snap;
    (void)str_copy(buf, " ⟳ ", PD_BUF);
    pd_cat(buf, ProgressDialog_title_for((int)s->op));
    pd_cat(buf, " ");
    pd_cat_u64(buf, s->percent);
    pd_cat(buf, "%");
    pd_cat(buf, "  ·  ");
    pd_format_state(s, buf);
}

void ProgressDialog_layout(Widget *self, int x, int y, int w, int h)
{
    int dw = w - 6;
    if (dw > PD_W_MAX) dw = PD_W_MAX;
    if (dw < PD_W_MIN) dw = w;
    int dh = PD_H;
    if (dh > h) dh = h;
    int dx = x + ((w - dw) >> 1);
    int dy = y + ((h - dh) >> 1);
    Widget_layout(self, dx, dy, dw, dh);
}

ProgressDialog *ProgressDialog_new(App *app)
{
    ProgressDialog *d = DUOP_NEW(ProgressDialog, ProgressDialog_vtbl);
    if (!d) return nullptr;
    Dialog_init(&d->base, app, nullptr, OP_PROGRESS);
    d->base.focus_btn = 1;
    ProgressDialog_layout(&d->base.base, 0, 0, (int)app->term_w, (int)app->term_h);
    return d;
}

static void pd_layout_buttons(ProgressDialog *d)
{
    const LangStrings *ls = i18n_get();
    App *app = (App *)(uintptr_t)d->base.app;
    const TaskSnap *s = (const TaskSnap *)(const void *)app->task_snap;
    const char **lbls = (const char **)(void *)d->btn_lbl;
    lbls[0] = s->paused ? ls->s[LS_task_btn_resume] : ls->s[LS_task_btn_pause];
    lbls[1] = ls->s[LS_task_btn_bg];
    lbls[2] = ls->s[LS_dlg_btn_cancel];
    int tot = 4;
    for (int i = 0; i < PD_BTNS; i++) tot += utf8_display_width(lbls[i]);
    int cx = d->base.base.x + ((d->base.base.w - tot) >> 1);
    int by = d->base.base.y + d->base.base.h - 2;
    d->base.btn_count = PD_BTNS;
    HitZone *z = (HitZone *)(void *)d->base.btns;
    for (int i = 0; i < PD_BTNS; i++) {
        int bw = utf8_display_width(lbls[i]);
        z[i].x1 = (int16_t)cx;
        cx += bw;
        z[i].x2 = (int16_t)cx;
        z[i].y = (int16_t)by;
        z[i].id = (int16_t)i;
        cx += 2;
    }
}

static void pd_draw_bar(ProgressDialog *d, ScreenBuffer *scr, const Theme *th, const TaskSnap *s)
{
    int x = d->base.base.x, y = d->base.base.y, bar_w = d->base.base.w - 11;
    if (bar_w <= 0) return;
    uint32_t pct = s->percent > 100u ? 100u : s->percent;
    int filled = (int)((pct * (uint32_t)bar_w) / 100u);
    ScreenBuffer_set_pen(scr, th->accent, th->dialog_bg, ATTR_NONE);
    ScreenBuffer_fill_rect(scr, x + 3, y + 3, filled, 1, CH_BLOCK_FULL);
    ScreenBuffer_set_pen(scr, th->muted, th->dialog_bg, ATTR_NONE);
    ScreenBuffer_fill_rect(scr, x + 3 + filled, y + 3, bar_w - filled, 1, CH_SHADE_LIGHT);
    char line[PD_BUF];
    line[0] = 0;
    pd_cat_u64(line, s->percent);
    pd_cat(line, "%");
    ScreenBuffer_set_pen(scr, th->text, th->dialog_bg, ATTR_BOLD);
    (void)ScreenBuffer_draw_text(scr, x + 4 + bar_w, y + 3, line, 4);
}

static void pd_draw_counts(ProgressDialog *d, ScreenBuffer *scr, const Theme *th, const TaskSnap *s)
{
    char line[PD_BUF];
    line[0] = 0;
    if (s->op != TASK_OP_DELETE) {
        pd_cat_size(line, (int64_t)s->done_bytes);
        if (s->total_bytes) {
            pd_cat(line, " / ");
            pd_cat_size(line, (int64_t)s->total_bytes);
        }
        pd_cat(line, "  ·  ");
    }
    pd_cat(line, i18n_get()->s[LS_task_lbl_files]);
    uint64_t df = s->done_files > s->total_files ? s->total_files : s->done_files;
    pd_cat_u64(line, df);
    pd_cat(line, " / ");
    pd_cat_u64(line, s->total_files);
    ScreenBuffer_set_pen(scr, th->text, th->dialog_bg, ATTR_NONE);
    (void)ScreenBuffer_draw_text(scr, d->base.base.x + 3, d->base.base.y + 4, line, d->base.base.w - 6);
}

static void pd_draw_buttons(ProgressDialog *d, ScreenBuffer *scr, const Theme *th)
{
    const HitZone *z = (const HitZone *)(const void *)d->base.btns;
    const char *const *lbls = (const char *const *)(const void *)d->btn_lbl;
    for (int i = 0; i < PD_BTNS; i++) {
        uint32_t fg = th->badge_fg, bg = th->badge_bg, attr = ATTR_NONE;
        if (i == (int)d->base.focus_btn) {
            fg = th->accent_fg; bg = th->accent; attr = ATTR_BOLD;
        } else if (i == (int)d->base.hovered_btn) {
            bg = th->badge_hover_bg; attr = ATTR_BOLD;
        }
        ScreenBuffer_set_pen(scr, fg, bg, attr);
        int x1 = (int16_t)z[i].x1;
        ScreenBuffer_draw_text_pad(scr, x1, (int16_t)z[i].y, lbls[i], (int16_t)z[i].x2 - x1);
    }
}

static void ProgressDialog_render(Widget *self, void *screen, void *theme)
{
    ProgressDialog *d = (ProgressDialog *)self;
    ScreenBuffer *scr = screen;
    const Theme *th = theme;
    App *app = (App *)(uintptr_t)d->base.app;
    const TaskSnap *s = (const TaskSnap *)(const void *)app->task_snap;
    int x = self->x, y = self->y, w = self->w, h = self->h;
    char line[PD_BUF];

    pd_layout_buttons(d);
    ScreenBuffer_set_pen(scr, th->dialog_border, th->dialog_bg, ATTR_BOLD);
    ScreenBuffer_draw_box(scr, x, y, w, h, 1);

    const char *title = ProgressDialog_title_for((int)s->op);
    d->base.title = (uint64_t)(uintptr_t)title;
    (void)str_copy(line, " ", PD_BUF);
    pd_cat(line, title);
    pd_cat(line, " ");
    ScreenBuffer_set_pen(scr, th->accent_fg, th->dialog_border, ATTR_BOLD);
    (void)ScreenBuffer_draw_text(scr, x + 3, y, line, w - 6);

    ScreenBuffer_set_pen(scr, th->text, th->dialog_bg, ATTR_BOLD);
    (void)ScreenBuffer_draw_text(scr, x + 3, y + 2, s->cur_name, w - 6);

    pd_draw_bar(d, scr, th, s);
    pd_draw_counts(d, scr, th, s);

    line[0] = 0;
    pd_format_state(s, line);
    uint32_t sfg = (s->paused | s->canceling) ? th->warn_fg : th->muted;
    uint32_t sattr = (s->paused | s->canceling) ? ATTR_BOLD : ATTR_NONE;
    ScreenBuffer_set_pen(scr, sfg, th->dialog_bg, sattr);
    (void)ScreenBuffer_draw_text(scr, x + 3, y + 5, line, w - 6);

    pd_draw_buttons(d, scr, th);
}

static void pd_activate(ProgressDialog *d, int idx)
{
    App *app = (App *)(uintptr_t)d->base.app;
    if (idx == 0) App_task_toggle_pause(app);
    else if (idx == 1) App_task_hide(app);
    else if (idx == 2) App_task_cancel(app);
}

static void ProgressDialog_on_hide(Dialog *self)
{
    App_task_hide((App *)(uintptr_t)self->app);
}

static int ProgressDialog_on_key(Widget *self, void *event)
{
    ProgressDialog *d = (ProgressDialog *)self;
    const Event *ev = event;
    uint32_t lat = ev->lat, mods = ev->mods;
    if (mods & MOD_ALT) {
        if (lat == 'j' || lat == 'J') pd_activate(d, 1);
        return 1;
    }
    if (mods & MOD_CTRL) return 1;
    switch (lat) {
    case KEY_ESC: case 'b': case 'B': pd_activate(d, 1); break;
    case ' ': case 'p': case 'P': pd_activate(d, 0); break;
    case 'c': case 'C': pd_activate(d, 2); break;
    case KEY_ENTER: pd_activate(d, (int)d->base.focus_btn); break;
    case KEY_TAB: case KEY_RIGHT:
        d->base.focus_btn = (d->base.focus_btn + 1u) % PD_BTNS;
        ((App *)(uintptr_t)d->base.app)->need_redraw = 1;
        break;
    case KEY_BTAB: case KEY_LEFT:
        d->base.focus_btn = (d->base.focus_btn + PD_BTNS - 1u) % PD_BTNS;
        ((App *)(uintptr_t)d->base.app)->need_redraw = 1;
        break;
    default: break;
    }
    return 1;
}

static int ProgressDialog_on_mouse(Widget *self, void *event)
{
    ProgressDialog *d = (ProgressDialog *)self;
    const Event *ev = event;
    int mx = ev->mouse_x, my = ev->mouse_y, hit = -1;
    const HitZone *z = (const HitZone *)(const void *)d->base.btns;
    for (uint32_t i = 0; i < d->base.btn_count; i++) {
        if (my == (int16_t)z[i].y && mx >= (int16_t)z[i].x1 && mx < (int16_t)z[i].x2) {
            hit = (int16_t)z[i].id;
            break;
        }
    }
    if (ev->mouse_kind == MACT_MOVE) {
        if (hit != (int)d->base.hovered_btn) {
            d->base.hovered_btn = (uint32_t)hit;
            ((App *)(uintptr_t)d->base.app)->need_redraw = 1;
        }
    } else if (ev->mouse_kind == MACT_PRESS && ev->mouse_btn == MOUSE_LEFT && hit >= 0) {
        pd_activate(d, hit);
    }
    return 1;
}
