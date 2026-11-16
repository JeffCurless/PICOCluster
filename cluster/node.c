/*
 * node.c — node identity, config.txt, WiFi join, and the peer table.
 */

#include "cluster/node.h"
#include "cluster/core.h"

#include "kernel/mem.h"
#include "kernel/vfs.h"
#include "kernel/wifi.h"

#include <string.h>

#define CL_CONFIG_FILE   "config.txt"
#define CL_CONFIG_BUFSZ  512u

static cl_node_t g_nodes[CL_MAX_NODES];

/* -------------------------------------------------------------------------
 * config.txt
 * ------------------------------------------------------------------------- */
static void copy_value(char *dst, size_t dst_sz, const char *src, size_t len)
{
    if (len >= dst_sz) {
        len = dst_sz - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool key_is(const char *line, size_t line_len, const char *key,
                   const char **val, size_t *val_len)
{
    size_t klen = strlen(key);
    if (line_len < klen || strncmp(line, key, klen) != 0) {
        return false;
    }
    *val     = line + klen;
    *val_len = line_len - klen;
    return true;
}

/* Read config.txt into buf (NUL-terminated).  Bytes read, or <= 0. */
static int read_config(char *buf, size_t buf_sz)
{
    int fd = vfs_open(CL_CONFIG_FILE, VFS_O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int n = vfs_read(fd, (uint8_t *)buf, (uint32_t)(buf_sz - 1u));
    vfs_close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

int cl_config_value(const char *key, char *out, size_t out_sz)
{
    if (key == NULL || out == NULL || out_sz == 0u) {
        return -1;
    }
    /* Heap, not a static: apps may call this while clusterd is reading
     * config.txt at boot. */
    char *buf = (char *)kmalloc(CL_CONFIG_BUFSZ);
    if (buf == NULL) {
        return -1;
    }
    int found = -1;
    if (read_config(buf, CL_CONFIG_BUFSZ) > 0) {
        const char *p = buf;
        while (*p) {
            const char *eol = p;
            while (*eol && *eol != '\n' && *eol != '\r') eol++;
            const char *v;
            size_t vlen;
            if (key_is(p, (size_t)(eol - p), key, &v, &vlen)) {
                copy_value(out, out_sz, v, vlen);
                found = (int)strlen(out);
                break;
            }
            p = eol;
            while (*p == '\n' || *p == '\r') p++;
        }
    }
    kfree(buf);
    return found;
}

/* OFF / NO / FALSE / 0 (any case) -> false; anything else -> true. */
static bool parse_on_off(const char *v, size_t len)
{
    static const char *const off[] = { "off", "no", "false", "0" };
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        size_t n = strlen(off[i]);
        if (len != n) continue;
        size_t k = 0;
        while (k < n && (v[k] | 0x20) == off[i][k]) k++;
        if (k == n) return false;
    }
    return true;
}

int cl_config_load(cl_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->nodeid = -1;
    cfg->status_page = true;

    static char buf[CL_CONFIG_BUFSZ];
    int n = read_config(buf, sizeof(buf));
    if (n < 0) {
        CL_LOG("cannot open %s\r\n", CL_CONFIG_FILE);
        return -1;
    }
    if (n == 0) {
        CL_LOG("%s is empty\r\n", CL_CONFIG_FILE);
        return -1;
    }

    const char *p = buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        size_t line_len = (size_t)(eol - p);
        const char *v;
        size_t vlen;

        /* The ALT keys first: "SSID=" is not a prefix of "SSIDALT=", but
         * keeping the longer keys first makes that obvious. */
        if (key_is(p, line_len, "SSIDALT=", &v, &vlen)) {
            copy_value(cfg->ssid_alt, sizeof(cfg->ssid_alt), v, vlen);
        } else if (key_is(p, line_len, "PASSWORDALT=", &v, &vlen)) {
            copy_value(cfg->pass_alt, sizeof(cfg->pass_alt), v, vlen);
        } else if (key_is(p, line_len, "SSID=", &v, &vlen)) {
            copy_value(cfg->ssid, sizeof(cfg->ssid), v, vlen);
        } else if (key_is(p, line_len, "PASSWORD=", &v, &vlen)) {
            copy_value(cfg->password, sizeof(cfg->password), v, vlen);
        } else if (key_is(p, line_len, "STATUS=", &v, &vlen)) {
            cfg->status_page = parse_on_off(v, vlen);
        } else if (key_is(p, line_len, "NODEID=", &v, &vlen)) {
            int id = 0;
            size_t i = 0;
            for (; i < vlen && v[i] >= '0' && v[i] <= '9'; i++) {
                id = id * 10 + (v[i] - '0');
                if (id > 65534) break;
            }
            cfg->nodeid = (i > 0u && i == vlen && id <= 65534) ? id : -1;
        }

        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }

    if (cfg->nodeid < 0) {
        CL_LOG("config.txt: NODEID=<0..65534> missing or invalid\r\n");
        return -1;
    }
    if (cfg->ssid[0] == '\0') {
        CL_LOG("config.txt: SSID= missing\r\n");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * WiFi
 * ------------------------------------------------------------------------- */
int cl_wifi_up(const cl_config_t *cfg)
{
    if (wifi_get_state() == WIFI_STATE_UP) {
        return 0;
    }
    CL_LOG("joining '%s'...\r\n", cfg->ssid);
    if (wifi_connect(cfg->ssid, cfg->password) == 0) {
        return 0;
    }
    if (cfg->ssid_alt[0] != '\0') {
        CL_LOG("joining alternate '%s'...\r\n", cfg->ssid_alt);
        if (wifi_connect(cfg->ssid_alt, cfg->pass_alt) == 0) {
            return 0;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Node table (caller holds cl_lock)
 * ------------------------------------------------------------------------- */
void cl_nodes_init(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
}

cl_node_t *cl_node_find(uint16_t id)
{
    for (int i = 0; i < CL_MAX_NODES; i++) {
        if (g_nodes[i].used && g_nodes[i].id == id) {
            return &g_nodes[i];
        }
    }
    return NULL;
}

cl_node_t *cl_node_slot(int idx)
{
    if (idx < 0 || idx >= CL_MAX_NODES) {
        return NULL;
    }
    return &g_nodes[idx];
}

bool cl_node_usable(const cl_node_t *n)
{
    return n != NULL && n->used && n->build_id == cl_build_id;
}

void cl_node_hello(uint16_t id, const char *ip, uint32_t build_id,
                   uint8_t free_slots, uint8_t max_slots, uint64_t now)
{
    cl_node_t *n = cl_node_find(id);
    if (n == NULL) {
        for (int i = 0; i < CL_MAX_NODES; i++) {
            if (!g_nodes[i].used) {
                n = &g_nodes[i];
                break;
            }
        }
        if (n == NULL) {
            return;                       /* table full — ignore */
        }
        memset(n, 0, sizeof(*n));
        n->used = true;
        n->id   = id;
        CL_LOG("node %u joined (%s)%s\r\n", (unsigned)id, ip,
               build_id == cl_build_id ? "" : " — INCOMPATIBLE build");
    } else if (strcmp(n->ip, ip) != 0) {
        CL_LOG("node %u moved %s -> %s\r\n", (unsigned)id, n->ip, ip);
    }
    strncpy(n->ip, ip, sizeof(n->ip) - 1u);
    n->ip[sizeof(n->ip) - 1u] = '\0';
    n->build_id     = build_id;
    n->max_slots    = max_slots;
    n->last_seen_us = now;
    /* Each HELLO arrives twice (unicast and multicast copies, see
     * send_hello in clusterd.c).  Take the slot counts from the first only:
     * a second reset of sent_since_hello would forget SPAWNs sent between
     * the copies. */
    if (n->last_hello_us == 0u || now - n->last_hello_us >= CL_HELLO_DEDUP_US) {
        n->free_slots       = free_slots;
        n->sent_since_hello = 0u;
        n->last_hello_us    = now;
    }
}

void cl_node_touch(uint16_t id, uint64_t now)
{
    cl_node_t *n = cl_node_find(id);
    if (n != NULL) {
        n->last_seen_us = now;
    }
}

int cl_nodes_expire(uint64_t now, uint16_t *lost, int max_lost)
{
    int count = 0;
    for (int i = 0; i < CL_MAX_NODES; i++) {
        cl_node_t *n = &g_nodes[i];
        if (!n->used || now - n->last_seen_us < CL_NODE_TIMEOUT_US) {
            continue;
        }
        CL_LOG("node %u lost (%s)\r\n", (unsigned)n->id, n->ip);
        if (n->build_id == cl_build_id && count < max_lost) {
            lost[count++] = n->id;
        }
        n->used = false;
    }
    return count;
}
