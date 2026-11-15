/*
 * proto.c — picoCluster wire format encode/decode (see proto.h).
 */

#include "proto.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Little-endian byte helpers (no unaligned access)
 * ------------------------------------------------------------------------- */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write the header; returns a pointer to the body, or NULL if it won't fit. */
static uint8_t *put_hdr(uint8_t *buf, size_t cap, cl_msg_type_t type,
                        size_t body_len, uint32_t build_id, uint16_t node)
{
    if (buf == NULL || CL_HDR_LEN + body_len > cap || body_len > 0xFFFFu) {
        return NULL;
    }
    put32(buf + 0, CL_MAGIC);
    buf[4] = (uint8_t)CL_PROTO_VER;
    buf[5] = (uint8_t)type;
    put16(buf + 6, (uint16_t)body_len);
    put32(buf + 8, build_id);
    put16(buf + 12, node);
    put16(buf + 14, 0u);
    return buf + CL_HDR_LEN;
}

/* -------------------------------------------------------------------------
 * Encoders
 * ------------------------------------------------------------------------- */
size_t cl_encode_hello(uint8_t *buf, size_t cap, uint32_t build_id,
                       uint16_t node, uint8_t free_slots, uint8_t max_slots)
{
    uint8_t *b = put_hdr(buf, cap, CL_MSG_HELLO, CL_HELLO_BODY, build_id, node);
    if (b == NULL) {
        return 0u;
    }
    b[0] = free_slots;
    b[1] = max_slots;
    b[2] = 0u;
    b[3] = 0u;
    return CL_HDR_LEN + CL_HELLO_BODY;
}

size_t cl_encode_spawn(uint8_t *buf, size_t cap, uint32_t build_id,
                       uint16_t node, uint32_t req_id, const char *task,
                       const void *arg, size_t arg_len)
{
    if (task == NULL || arg_len > CLUSTER_MAX_PAYLOAD ||
        (arg_len > 0u && arg == NULL)) {
        return 0u;
    }
    size_t name_len = strlen(task);
    if (name_len == 0u || name_len >= CL_TASK_NAME_MAX) {
        return 0u;
    }
    size_t body = CL_SPAWN_FIXED + arg_len;
    uint8_t *b = put_hdr(buf, cap, CL_MSG_SPAWN, body, build_id, node);
    if (b == NULL) {
        return 0u;
    }
    put32(b, req_id);
    memset(b + 4, 0, CL_TASK_NAME_MAX);
    memcpy(b + 4, task, name_len);
    if (arg_len > 0u) {
        memcpy(b + CL_SPAWN_FIXED, arg, arg_len);
    }
    return CL_HDR_LEN + body;
}

size_t cl_encode_spawn_ack(uint8_t *buf, size_t cap, uint32_t build_id,
                           uint16_t node, uint32_t req_id, uint8_t code)
{
    uint8_t *b = put_hdr(buf, cap, CL_MSG_SPAWN_ACK, CL_ACK_BODY, build_id, node);
    if (b == NULL) {
        return 0u;
    }
    put32(b, req_id);
    b[4] = code;
    b[5] = 0u;
    b[6] = 0u;
    b[7] = 0u;
    return CL_HDR_LEN + CL_ACK_BODY;
}

size_t cl_encode_result(uint8_t *buf, size_t cap, uint32_t build_id,
                        uint16_t node, uint32_t req_id, int32_t status,
                        int32_t err, const void *ret, size_t ret_len)
{
    if (ret_len > CLUSTER_MAX_PAYLOAD || (ret_len > 0u && ret == NULL)) {
        return 0u;
    }
    size_t body = CL_RESULT_FIXED + ret_len;
    uint8_t *b = put_hdr(buf, cap, CL_MSG_RESULT, body, build_id, node);
    if (b == NULL) {
        return 0u;
    }
    put32(b + 0, req_id);
    put32(b + 4, (uint32_t)status);
    put32(b + 8, (uint32_t)err);
    if (ret_len > 0u) {
        memcpy(b + CL_RESULT_FIXED, ret, ret_len);
    }
    return CL_HDR_LEN + body;
}

size_t cl_encode_result_ack(uint8_t *buf, size_t cap, uint32_t build_id,
                            uint16_t node, uint32_t req_id)
{
    uint8_t *b = put_hdr(buf, cap, CL_MSG_RESULT_ACK, CL_RACK_BODY, build_id, node);
    if (b == NULL) {
        return 0u;
    }
    put32(b, req_id);
    return CL_HDR_LEN + CL_RACK_BODY;
}

size_t cl_encode_kill(uint8_t *buf, size_t cap, uint32_t build_id,
                      uint16_t node, cl_msg_type_t type, uint32_t req_id)
{
    if (type != CL_MSG_KILL && type != CL_MSG_KILL_ACK) {
        return 0u;
    }
    uint8_t *b = put_hdr(buf, cap, type, CL_KILL_BODY, build_id, node);
    if (b == NULL) {
        return 0u;
    }
    put32(b, req_id);
    return CL_HDR_LEN + CL_KILL_BODY;
}

/* -------------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------------- */
bool cl_decode(const uint8_t *buf, size_t len, cl_msg_t *out)
{
    if (buf == NULL || out == NULL || len < CL_HDR_LEN) {
        return false;
    }
    if (get32(buf) != CL_MAGIC || buf[4] != CL_PROTO_VER) {
        return false;
    }
    size_t body_len = get16(buf + 6);
    if (CL_HDR_LEN + body_len != len) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->type     = (cl_msg_type_t)buf[5];
    out->build_id = get32(buf + 8);
    out->src_node = get16(buf + 12);
    const uint8_t *b = buf + CL_HDR_LEN;

    switch (out->type) {
    case CL_MSG_HELLO:
        if (body_len != CL_HELLO_BODY) return false;
        out->free_slots = b[0];
        out->max_slots  = b[1];
        return true;

    case CL_MSG_SPAWN:
        if (body_len < CL_SPAWN_FIXED ||
            body_len - CL_SPAWN_FIXED > CLUSTER_MAX_PAYLOAD) {
            return false;
        }
        out->req_id = get32(b);
        memcpy(out->task, b + 4, CL_TASK_NAME_MAX);
        out->task[CL_TASK_NAME_MAX - 1u] = '\0';
        if (out->task[0] == '\0') return false;
        out->data     = b + CL_SPAWN_FIXED;
        out->data_len = (uint16_t)(body_len - CL_SPAWN_FIXED);
        return true;

    case CL_MSG_SPAWN_ACK:
        if (body_len != CL_ACK_BODY) return false;
        out->req_id = get32(b);
        out->code   = b[4];
        return true;

    case CL_MSG_RESULT:
        if (body_len < CL_RESULT_FIXED ||
            body_len - CL_RESULT_FIXED > CLUSTER_MAX_PAYLOAD) {
            return false;
        }
        out->req_id   = get32(b + 0);
        out->status   = (int32_t)get32(b + 4);
        out->err      = (int32_t)get32(b + 8);
        out->data     = b + CL_RESULT_FIXED;
        out->data_len = (uint16_t)(body_len - CL_RESULT_FIXED);
        return true;

    case CL_MSG_RESULT_ACK:
        if (body_len != CL_RACK_BODY) return false;
        out->req_id = get32(b);
        return true;

    case CL_MSG_KILL:
    case CL_MSG_KILL_ACK:
        if (body_len != CL_KILL_BODY) return false;
        out->req_id = get32(b);
        return true;

    default:
        return false;
    }
}

/* -------------------------------------------------------------------------
 * FNV-1a
 * ------------------------------------------------------------------------- */
uint32_t cl_fnv1a(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* -------------------------------------------------------------------------
 * Recent request ids
 * ------------------------------------------------------------------------- */
void cl_recent_init(cl_recent_t *r)
{
    memset(r, 0, sizeof(*r));
}

void cl_recent_add(cl_recent_t *r, uint32_t id)
{
    r->ids[r->next] = id;
    r->next = (r->next + 1u) % CL_RECENT_MAX;
    if (r->count < CL_RECENT_MAX) {
        r->count++;
    }
}

bool cl_recent_has(const cl_recent_t *r, uint32_t id)
{
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->ids[i] == id) {
            return true;
        }
    }
    return false;
}
