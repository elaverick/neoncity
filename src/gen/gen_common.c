#include "gen_common.h"
#include <stdio.h>
#include <string.h>

void blog_vprintf(BLog *log, const char *fmt, va_list ap) {
    if (!log || !log->buf || log->len >= log->cap - 1) return;
    int n = vsnprintf(log->buf + log->len, (size_t)(log->cap - log->len), fmt, ap);
    if (n > 0) log->len = log->len + n < log->cap - 1 ? log->len + n : log->cap - 1;
}

void blog_printf(BLog *log, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    blog_vprintf(log, fmt, ap);
    va_end(ap);
}

void loop_to_frame(int q, const BLoop *in, BLoop *out) {
    memset(out, 0, sizeof(*out));
    out->n = in->n;
    for (int i = 0; i < in->n; ++i) quarter_turn(4 - q, in->x[i], in->z[i], &out->x[i], &out->z[i]);
}

void foot_to_frame(int q, const BFoot *in, BFoot *out) {
    loop_to_frame(q, &in->outer, &out->outer);
    loop_to_frame(q, &in->hole, &out->hole);
}
