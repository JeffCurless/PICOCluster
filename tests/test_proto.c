/*
 * tests/test_proto.c — host-native tests for the cluster wire format and the
 * recent-request filter (cluster/proto.c).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "proto.h"
#include "framework.h"

static uint8_t buf[CL_MAX_PKT + 8];

static void test_hello(void)
{
    BEGIN_TEST(hello_round_trip);
    size_t n = cl_encode_hello(buf, sizeof(buf), 0xA1B2C3D4u, 7u, 2u, 3u);
    CHECK(n == CL_HDR_LEN + CL_HELLO_BODY, "hello length");
    cl_msg_t m;
    CHECK(cl_decode(buf, n, &m), "decode");
    CHECK(m.type == CL_MSG_HELLO, "type");
    CHECK(m.build_id == 0xA1B2C3D4u, "build id");
    CHECK(m.src_node == 7u, "src node");
    CHECK(m.free_slots == 2u && m.max_slots == 3u, "slots");
    END_TEST();
}

static void test_spawn(void)
{
    BEGIN_TEST(spawn_round_trip);
    const char arg[] = "hello";
    size_t n = cl_encode_spawn(buf, sizeof(buf), 1u, 2u, 0x00020005u,
                               "echo", arg, sizeof(arg));
    CHECK(n == CL_HDR_LEN + CL_SPAWN_FIXED + sizeof(arg), "spawn length");
    cl_msg_t m;
    CHECK(cl_decode(buf, n, &m), "decode");
    CHECK(m.type == CL_MSG_SPAWN, "type");
    CHECK(m.req_id == 0x00020005u, "req id");
    CHECK(strcmp(m.task, "echo") == 0, "task name");
    CHECK(m.data_len == sizeof(arg), "arg length");
    CHECK(memcmp(m.data, arg, sizeof(arg)) == 0, "arg bytes");
    END_TEST();
}

static void test_spawn_limits(void)
{
    BEGIN_TEST(spawn_limits);
    static uint8_t big[CLUSTER_MAX_PAYLOAD + 1u];
    CHECK(cl_encode_spawn(buf, sizeof(buf), 1u, 2u, 3u, "t", big,
                          CLUSTER_MAX_PAYLOAD) != 0u, "max payload fits");
    CHECK(cl_encode_spawn(buf, sizeof(buf), 1u, 2u, 3u, "t", big,
                          CLUSTER_MAX_PAYLOAD + 1u) == 0u, "oversize rejected");
    CHECK(cl_encode_spawn(buf, sizeof(buf), 1u, 2u, 3u, "0123456789abcdef",
                          NULL, 0u) == 0u, "16-char name rejected");
    CHECK(cl_encode_spawn(buf, sizeof(buf), 1u, 2u, 3u, "",
                          NULL, 0u) == 0u, "empty name rejected");
    CHECK(cl_encode_spawn(buf, 20u, 1u, 2u, 3u, "t", NULL, 0u) == 0u,
          "small buffer rejected");
    END_TEST();
}

static void test_ack_result(void)
{
    BEGIN_TEST(ack_and_result);
    cl_msg_t m;
    size_t n = cl_encode_spawn_ack(buf, sizeof(buf), 1u, 4u, 99u, CL_ACK_BUSY);
    CHECK(cl_decode(buf, n, &m) && m.type == CL_MSG_SPAWN_ACK, "ack decode");
    CHECK(m.req_id == 99u && m.code == CL_ACK_BUSY, "ack fields");

    const uint8_t ret[3] = { 1, 2, 3 };
    n = cl_encode_result(buf, sizeof(buf), 1u, 4u, 99u, -42, -7, ret, 3u);
    CHECK(cl_decode(buf, n, &m) && m.type == CL_MSG_RESULT, "result decode");
    CHECK(m.status == -42 && m.err == -7, "negative status/err survive");
    CHECK(m.data_len == 3u && memcmp(m.data, ret, 3u) == 0, "result bytes");

    n = cl_encode_result_ack(buf, sizeof(buf), 1u, 4u, 99u);
    CHECK(cl_decode(buf, n, &m) && m.type == CL_MSG_RESULT_ACK, "rack decode");
    CHECK(m.req_id == 99u, "rack req id");
    END_TEST();
}

static void test_kill(void)
{
    BEGIN_TEST(kill_and_kill_ack);
    cl_msg_t m;
    size_t n = cl_encode_kill(buf, sizeof(buf), 1u, 3u, CL_MSG_KILL, 0x00030009u);
    CHECK(n == CL_HDR_LEN + CL_KILL_BODY, "kill length");
    CHECK(cl_decode(buf, n, &m) && m.type == CL_MSG_KILL, "kill decode");
    CHECK(m.req_id == 0x00030009u && m.src_node == 3u, "kill fields");
    n = cl_encode_kill(buf, sizeof(buf), 1u, 3u, CL_MSG_KILL_ACK, 7u);
    CHECK(cl_decode(buf, n, &m) && m.type == CL_MSG_KILL_ACK, "kill ack decode");
    CHECK(m.req_id == 7u, "kill ack req id");
    CHECK(cl_encode_kill(buf, sizeof(buf), 1u, 3u, CL_MSG_SPAWN, 7u) == 0u,
          "other types rejected");
    END_TEST();
}

static void test_reject(void)
{
    BEGIN_TEST(decode_rejects_bad_packets);
    cl_msg_t m;
    size_t n = cl_encode_hello(buf, sizeof(buf), 1u, 2u, 3u, 3u);
    CHECK(!cl_decode(buf, n - 1u, &m), "truncated");
    CHECK(!cl_decode(buf, CL_HDR_LEN - 1u, &m), "shorter than header");

    buf[0] ^= 0xFFu;
    CHECK(!cl_decode(buf, n, &m), "bad magic");
    buf[0] ^= 0xFFu;

    buf[4] = (uint8_t)(CL_PROTO_VER + 1u);
    CHECK(!cl_decode(buf, n, &m), "bad version");
    buf[4] = (uint8_t)CL_PROTO_VER;

    buf[5] = 99u;
    CHECK(!cl_decode(buf, n, &m), "unknown type");
    buf[5] = (uint8_t)CL_MSG_HELLO;
    CHECK(cl_decode(buf, n, &m), "restored packet decodes");

    /* A SPAWN whose name field has no NUL is still terminated safely. */
    n = cl_encode_spawn(buf, sizeof(buf), 1u, 2u, 3u, "abc", NULL, 0u);
    memset(buf + CL_HDR_LEN + 4u, 'x', CL_TASK_NAME_MAX);
    CHECK(cl_decode(buf, n, &m), "unterminated name decodes");
    CHECK(strlen(m.task) == CL_TASK_NAME_MAX - 1u, "name forced to terminate");
    END_TEST();
}

static void test_recent(void)
{
    BEGIN_TEST(recent_ids);
    cl_recent_t r;
    cl_recent_init(&r);
    CHECK(!cl_recent_has(&r, 0u), "empty table has no id 0");
    for (uint32_t i = 1; i <= CL_RECENT_MAX; i++) {
        cl_recent_add(&r, i);
    }
    CHECK(cl_recent_has(&r, 1u) && cl_recent_has(&r, CL_RECENT_MAX), "all kept");
    cl_recent_add(&r, 1000u);
    CHECK(!cl_recent_has(&r, 1u), "oldest evicted");
    CHECK(cl_recent_has(&r, 2u) && cl_recent_has(&r, 1000u), "rest kept");
    END_TEST();
}

static void test_fnv(void)
{
    BEGIN_TEST(fnv1a_known_values);
    CHECK(cl_fnv1a(CL_FNV_INIT, "", 0u) == 0x811C9DC5u, "empty");
    CHECK(cl_fnv1a(CL_FNV_INIT, "a", 1u) == 0xE40C292Cu, "\"a\"");
    CHECK(cl_fnv1a(CL_FNV_INIT, "foobar", 6u) == 0xBF9CF968u, "\"foobar\"");
    END_TEST();
}

int main(void)
{
    printf("cluster proto tests\n");
    test_hello();
    test_spawn();
    test_spawn_limits();
    test_ack_result();
    test_kill();
    test_reject();
    test_recent();
    test_fnv();
    SUMMARY();
}
