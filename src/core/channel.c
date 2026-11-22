#include "finte/object.h"

static fthread_t *wait_pop(fthread_t **head)
{
    fthread_t *t = *head;
    if (t) {
        *head = t->wait_next;
        t->wait_next = NULL;
    }
    return t;
}

void finte_chan_release_queue(fchan_t *ch)
{
    if (!ch) {
        return;
    }
    for (uint16_t i = 0; i < ch->count; i++) {
        uint16_t idx = (uint16_t)((ch->head + i) % ch->capacity);
        for (uint8_t c = 0; c < ch->qcap_count[idx]; c++) {
            if (finte_handle_valid(ch->qcaps[idx][c].handle)) {
                finte_node_unref(ch->qcaps[idx][c].handle);
            }
            ch->qcaps[idx][c].handle = FINTE_HANDLE_NONE;
        }
        ch->qcap_count[idx] = 0;
    }
    ch->count = 0;
    ch->head = 0;
    ch->tail = 0;
}

fret_t finte_chan_send(fflow_t *f, uint16_t cap, fmsg_t *msg, uint32_t flags)
{
    if (!f || !msg) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    if (msg->len > FINTE_MSG_DATA || msg->cap_count > FINTE_MSG_CAPS) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    fcap_t c;
    int rc = finte_cap_check(f, cap, FINTE_R_SEND, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_CHANNEL) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    fchan_t *ch = &finte_core.channels[n->index];

    /* Resolve attached caps before we block, so a bad message cannot be
     * enqueued after an arbitrary wait. */
    fcap_t attach[FINTE_MSG_CAPS];
    for (uint8_t i = 0; i < msg->cap_count; i++) {
        int r = finte_cap_check(f, msg->caps[i], 0, &attach[i]);
        if (r < 0) {
            return finte_ret(r, 0);
        }
    }

    for (;;) {
        if (f->abort) {
            return finte_ret(FINTE_ERR_STATE, 0);
        }
        if (ch->closed) {
            return finte_ret(FINTE_ERR_STATE, 0);
        }
        if (ch->count < ch->capacity) {
            break;
        }
        if (flags & FINTE_MSG_NONBLOCK) {
            return finte_ret(FINTE_ERR_AGAIN, 0);
        }
        finte_flow_block(f, ch, 1);
    }

    uint16_t at = ch->tail;
    finte_memcpy(&ch->ring[at], msg, sizeof(*msg));
    ch->qcap_count[at] = msg->cap_count;
    for (uint8_t i = 0; i < msg->cap_count; i++) {
        ch->qcaps[at][i] = attach[i];
        finte_node_ref(attach[i].handle);
    }
    ch->tail = (uint16_t)((ch->tail + 1u) % ch->capacity);
    ch->count++;
    ch->sent++;
    finte_core.stats.messages_sent++;
    f->stats.messages_sent++;

    if (ch->wait_recv) {
        fthread_t *w = wait_pop(&ch->wait_recv);
        if (w) {
            finte_sched_wake_thread(w);
        }
    }
    return finte_ret_ok(0);
}

fret_t finte_chan_recv(fflow_t *f, uint16_t cap, fmsg_t *out, uint32_t flags)
{
    if (!f || !out) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    fcap_t c;
    int rc = finte_cap_check(f, cap, FINTE_R_RECV, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_CHANNEL) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    fchan_t *ch = &finte_core.channels[n->index];

    for (;;) {
        if (f->abort) {
            return finte_ret(FINTE_ERR_STATE, 0);
        }
        if (ch->count > 0) {
            break;
        }
        if (ch->closed) {
            return finte_ret(FINTE_ERR_EOF, 0);
        }
        if (flags & FINTE_MSG_NONBLOCK) {
            return finte_ret(FINTE_ERR_AGAIN, 0);
        }
        finte_flow_block(f, ch, 0);
    }

    uint16_t at = ch->head;
    uint8_t ncap = ch->qcap_count[at];
    if (finte_cap_free_slots(f) < (int)ncap) {
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }

    finte_memcpy(out, &ch->ring[at], sizeof(*out));
    out->cap_count = ncap;
    for (uint8_t i = 0; i < ncap; i++) {
        int slot = finte_cap_install_cap(f, &ch->qcaps[at][i]);
        if (slot < 0) {
            for (uint8_t j = 0; j < i; j++) {
                finte_cap_drop_index(f, out->caps[j]);
            }
            return finte_ret(FINTE_ERR_NOMEM, 0);
        }
        out->caps[i] = (uint16_t)slot;
        finte_node_unref(ch->qcaps[at][i].handle);
        ch->qcaps[at][i].handle = FINTE_HANDLE_NONE;
    }
    ch->qcap_count[at] = 0;
    ch->count--;
    ch->head = (uint16_t)((ch->head + 1u) % ch->capacity);
    ch->received++;
    finte_core.stats.messages_received++;
    f->stats.messages_received++;

    if (ch->wait_send) {
        fthread_t *w = wait_pop(&ch->wait_send);
        if (w) {
            finte_sched_wake_thread(w);
        }
    }
    return finte_ret_ok(0);
}

fret_t finte_chan_close(fflow_t *f, uint16_t cap)
{
    if (!f) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    fcap_t c;
    int rc = finte_cap_check(f, cap, FINTE_R_CONTROL, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_CHANNEL) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    fchan_t *ch = &finte_core.channels[n->index];
    ch->closed = 1;

    while (ch->wait_recv) {
        fthread_t *w = wait_pop(&ch->wait_recv);
        if (w) {
            finte_sched_wake_thread(w);
        }
    }
    while (ch->wait_send) {
        fthread_t *w = wait_pop(&ch->wait_send);
        if (w) {
            finte_sched_wake_thread(w);
        }
    }
    return finte_ret_ok(0);
}
