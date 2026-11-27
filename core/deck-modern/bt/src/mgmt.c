/* mgmt.c - the kernel's Bluetooth "management" socket.
 *
 * Every message (both directions) starts with 6 bytes:
 *     opcode (2) | controller index (2) | length of the rest (2)
 * We send COMMANDS (power on, start scan, pair...). The kernel answers each
 * one with a "command complete" (or "command status") EVENT carrying the same
 * opcode and a status byte (0 = success). Other events (device found, new key,
 * passkey to show...) can arrive at any time; they go to m->on_event. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include "bt.h"

int mgmt_open(struct mgmt *m)
{
    memset(m, 0, sizeof(*m));
    dbg("opening the management socket");
    m->fd = io_mgmt_socket();
    return m->fd < 0 ? -1 : 0;
}

void mgmt_close(struct mgmt *m)
{
    if (m->fd >= 0) close(m->fd);
    m->fd = -1;
}

int mgmt_send(struct mgmt *m, uint16_t op, uint16_t index, const void *p, uint16_t len)
{
    uint8_t buf[1024];
    if (len > sizeof(buf) - 6) return -1;
    put_le16(buf, op);
    put_le16(buf + 2, index);
    put_le16(buf + 4, len);
    if (len) memcpy(buf + 6, p, len);
    dbg("mgmt send op=0x%04x index=%u len=%u", op, index, len);
    if (write(m->fd, buf, 6 + len) != 6 + len) return -1;
    return 0;
}

/* The reply we are waiting for (if any) is stored here by handle_message(). */
struct pending {
    int active, done;
    uint16_t op, index;
    int status;
    uint8_t *rsp;
    uint16_t *rsp_len;
};
static struct pending g_wait;

static void handle_message(struct mgmt *m, const uint8_t *buf, int n)
{
    uint16_t ev, index, len;
    if (n < 6) return;
    ev = get_le16(buf);
    index = get_le16(buf + 2);
    len = get_le16(buf + 4);
    if (len > n - 6) return;                /* damaged message */
    dbg("mgmt event 0x%04x index=%u len=%u", ev, index, len);
    if ((ev == MGMT_EV_CMD_COMPLETE || ev == MGMT_EV_CMD_STATUS) && len >= 3 &&
        g_wait.active && !g_wait.done &&
        get_le16(buf + 6) == g_wait.op && index == g_wait.index) {
        g_wait.done = 1;
        g_wait.status = buf[8];
        if (g_wait.rsp && g_wait.rsp_len) {
            uint16_t extra = (ev == MGMT_EV_CMD_COMPLETE) ? len - 3 : 0;
            if (extra > *g_wait.rsp_len) extra = *g_wait.rsp_len;
            memcpy(g_wait.rsp, buf + 9, extra);
            *g_wait.rsp_len = extra;
        }
        return;
    }
    /* Events of every controller reach every management socket. Only ours
     * count (with two controllers we would otherwise save the OTHER one's
     * keys or answer its pairing questions). */
    if (m->filter && index != m->index && index != MGMT_INDEX_NONE) {
        dbg("ignored event 0x%04x of controller %u", ev, index);
        return;
    }
    if (m->on_event)
        m->on_event(m, ev, index, buf + 6, len);
}

/* Reads one message, waiting up to timeout_ms.
 * Returns 1 = handled one, 0 = nothing came, -1 = socket error/closed. */
int mgmt_read_one(struct mgmt *m, int timeout_ms)
{
    uint8_t buf[4096];
    struct pollfd pfd = { m->fd, POLLIN, 0 };
    int n, r = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
    n = read(m->fd, buf, sizeof(buf));
    if (n <= 0) return -1;
    handle_message(m, buf, n);
    return 1;
}

int mgmt_request(struct mgmt *m, uint16_t op, uint16_t index, const void *p, uint16_t len,
                 uint8_t *rsp, uint16_t *rsp_len, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    struct pending saved = g_wait;          /* (requests may nest inside event handlers) */
    int status;
    memset(&g_wait, 0, sizeof(g_wait));
    g_wait.active = 1;
    g_wait.op = op;
    g_wait.index = index;
    g_wait.rsp = rsp;
    g_wait.rsp_len = rsp_len;
    if (mgmt_send(m, op, index, p, len) != 0) {
        g_wait = saved;
        return MGMT_ERR_IO;
    }
    status = MGMT_ERR_TIMEOUT;
    while (!g_wait.done) {
        long long left = end - now_ms();
        if (left <= 0) break;
        if (mgmt_read_one(m, (int)left) < 0) { status = MGMT_ERR_IO; break; }
    }
    if (g_wait.done) status = g_wait.status;
    else if (rsp_len) *rsp_len = 0;
    g_wait = saved;
    return status;
}

/* Handles events for timeout_ms (e.g. while scanning). -1 if the socket died. */
int mgmt_wait(struct mgmt *m, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    for (;;) {
        long long left = end - now_ms();
        if (left <= 0) return 0;
        if (mgmt_read_one(m, (int)left) < 0) return -1;
    }
}

const char *mgmt_status_str(int status)
{
    static const char *names[] = {
        "success", "unknown command", "not connected", "failed", "connect failed",
        "authentication failed (wrong passkey/PIN?)", "not paired", "no resources",
        "timed out", "already connected", "busy", "rejected", "not supported",
        "invalid parameters", "disconnected", "not powered (run: nzeus-bt on)",
        "cancelled", "invalid index (no such controller)", "blocked by rfkill",
        "already paired", "permission denied (run as root)"
    };
    if (status >= 0 && status < (int)(sizeof(names) / sizeof(names[0]))) return names[status];
    if (status == MGMT_ERR_TIMEOUT) return "no answer from the kernel (timeout)";
    if (status == MGMT_ERR_IO) return "management socket error";
    return "unknown error";
}

/* Which controller to use: $NZEUS_BT_INDEX if set, else the first one the
 * kernel lists (hci0 on the console). Returns -1 if there is none. */
int mgmt_pick_index(struct mgmt *m)
{
    uint8_t rsp[512];
    uint16_t len = sizeof(rsp);
    const char *env = getenv("NZEUS_BT_INDEX");
    int st, count, i;
    st = mgmt_request(m, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, NULL, 0, rsp, &len, 3000);
    if (st != 0 || len < 2) return -1;
    count = get_le16(rsp);
    if (count == 0 || len < 2 + 2 * count) return -1;
    if (env && *env) {
        int want = atoi(env);
        for (i = 0; i < count; i++)
            if (get_le16(rsp + 2 + 2 * i) == want) { m->index = want; m->filter = 1; return want; }
        return -1;
    }
    m->index = get_le16(rsp + 2);
    m->filter = 1;
    return m->index;
}
