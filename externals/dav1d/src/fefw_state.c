/* Fortune's Eden: a one-line description of dav1d's frame-threading state, for the log when the
 * video decode thread has waited on dav1d for a long time (issue #22). Read without dav1d's
 * locks: dav1d is stuck when this is called, and taking its lock could stick the caller too. */

#include "config.h"

#include <stdio.h>
#include <stddef.h>

#include "src/internal.h"

#define FEFW_PUT(...)                                                                    \
    do {                                                                                 \
        if (o < n) {                                                                     \
            const int w = snprintf(buf + o, n - o, __VA_ARGS__);                         \
            if (w > 0) o += (size_t)w;                                                   \
        }                                                                                \
    } while (0)

static unsigned progress_of(const Dav1dThreadPicture *const p, const int i) {
    if (!p->p.data[0]) return 0xFFFFFFFEU; /* no picture */
    if (!p->progress) return 0xFFFFFFFDU; /* no progress counters */
    return atomic_load((atomic_uint *)&p->progress[i]);
}

size_t dav1d_fefw_describe(Dav1dContext *const c, char *const buf, const size_t n) {
    size_t o = 0;
    if (!c || !buf || !n) return 0;
    buf[0] = 0;
    FEFW_PUT("dav1d: frames %u, threads %u, next %u, first %u, cur %u, reset_cur %u, "
             "cond_signaled %d, drain %d, cached tiles %d, in %zu bytes",
             c->n_fc, c->n_tc, c->frame_thread.next,
             (unsigned)atomic_load(&c->task_thread.first), c->task_thread.cur,
             (unsigned)atomic_load(&c->task_thread.reset_task_cur),
             (int)atomic_load(&c->task_thread.cond_signaled), c->drain, c->n_tile_data,
             c->in.sz);
    for (unsigned i = 0; i < c->n_fc; i++) {
        Dav1dFrameContext *const f = &c->fc[i];
        int queued = 0;
        FEFW_PUT(" || frame %u: tiles %d, init_done %d, done %d/%d, error %d, retval %d, "
                 "task_counter %d, merge %d, pending %s, offset %d, type %d, show %d, "
                 "progress %u/%u, out_delayed %s",
                 i, f->n_tile_data, (int)atomic_load(&f->task_thread.init_done),
                 (int)atomic_load(&f->task_thread.done[0]),
                 (int)atomic_load(&f->task_thread.done[1]),
                 (int)atomic_load(&f->task_thread.error), f->task_thread.retval,
                 (int)atomic_load(&f->task_thread.task_counter),
                 (int)atomic_load(&f->task_thread.pending_tasks.merge),
                 f->task_thread.pending_tasks.head ? "yes" : "no",
                 f->frame_hdr ? (int)f->frame_hdr->frame_offset : -1,
                 f->frame_hdr ? (int)f->frame_hdr->frame_type : -1,
                 f->frame_hdr ? f->frame_hdr->show_frame : -1,
                 progress_of(&f->sr_cur, 0), progress_of(&f->sr_cur, 1),
                 (c->frame_thread.out_delayed && c->frame_thread.out_delayed[i].p.data[0])
                     ? "yes" : "no");
        FEFW_PUT(", refs");
        for (int r = 0; r < 7; r++) {
            const unsigned p = progress_of(&f->refp[r], 1);
            if (p == 0xFFFFFFFEU) FEFW_PUT(" -");
            else FEFW_PUT(" %u", p);
        }
        FEFW_PUT(", queue");
        for (const Dav1dTask *t = f->task_thread.task_head; t && queued < 12; t = t->next, queued++) {
            FEFW_PUT(" %d@%d(r%d,d%d,s%d)", (int)t->type, t->sby, t->recon_progress,
                     t->deblock_progress, t->deps_skip);
        }
        if (queued == 12) FEFW_PUT(" ...");
    }
    return o;
}

/* Wakes dav1d's worker threads and has them look through every frame's tasks again from the
 * start. Workers that found nothing to do sleep until another thread signals progress; on many
 * cores that signal is sometimes lost (dav1d issue #444): a frame keeps runnable tasks, every
 * worker sleeps, and dav1d_get_picture / dav1d_send_data wait forever. Done under dav1d's task
 * lock and the same way as its own reset_task_cur, so it is harmless when nothing is stuck. */
void dav1d_fefw_kick(Dav1dContext *const c) {
    if (!c || c->n_tc <= 1 || !c->task_thread.inited) return;
    struct TaskThreadData *const ttd = &c->task_thread;
    pthread_mutex_lock(&ttd->lock);
    ttd->cur = 0;
    for (unsigned i = 0; i < c->n_fc; i++)
        c->fc[i].task_thread.task_cur_prev = NULL;
    atomic_store(&ttd->cond_signaled, 1);
    pthread_cond_broadcast(&ttd->cond);
    pthread_mutex_unlock(&ttd->lock);
}
