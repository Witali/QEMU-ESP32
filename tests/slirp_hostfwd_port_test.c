/* SPDX-License-Identifier: BSD-3-Clause */
#include "slirp.h"
#include "hostfwd_port.h"

typedef struct Fixture {
    Slirp slirp;
    int64_t now_ns;
} Fixture;

typedef struct Input {
    const uint8_t *data;
    size_t size;
    size_t offset;
} Input;

/* The test controls guest time; host wall time is never advanced or slept. */
static int64_t test_clock(void *opaque)
{
    return ((Fixture *)opaque)->now_ns;
}

/* Create only the bookkeeping required by the pure port-lease component. */
static void fixture_init(Fixture *fixture)
{
    static const SlirpCb callbacks = { .clock_get_ns = test_clock };
    memset(fixture, 0, sizeof(*fixture));
    fixture->slirp.cb = &callbacks;
    fixture->slirp.opaque = fixture;
    fixture->slirp.tcb.so_next = fixture->slirp.tcb.so_prev = &fixture->slirp.tcb;
}

/* Describe a host-forwarded TCP tuple without opening any real socket. */
static void socket_init(struct socket *so, Fixture *fixture, uint16_t port)
{
    memset(so, 0, sizeof(*so));
    so->slirp = &fixture->slirp;
    so->so_ffamily = so->so_lfamily = AF_INET;
    so->so_faddr.s_addr = htonl(0x0a000202);
    so->so_laddr.s_addr = htonl(0x0a00020f);
    so->so_fport = htons(port);
    so->so_lport = htons(80);
}

/* Save exactly the emitted bytes, including the full-state wrapper when enabled. */
static slirp_ssize_t write_bytes(const void *data, size_t size, void *opaque)
{
    g_byte_array_append(opaque, data, size);
    return size;
}

/* Return an actual short read at EOF, exercising the decoder's bounds checks. */
static slirp_ssize_t read_bytes(void *data, size_t size, void *opaque)
{
    Input *input = opaque;
    size_t count = MIN(size, input->size - input->offset);
    memcpy(data, input->data + input->offset, count);
    input->offset += count;
    return count;
}

/* Obtain deterministic component state for both round trips and rejection guards. */
static GByteArray *save_pool(Fixture *fixture)
{
    GByteArray *bytes = g_byte_array_new();
    g_assert_cmpint(slirp_hostfwd_ports_save(&fixture->slirp, write_bytes, bytes), ==, 0);
    return bytes;
}

/* Original ports remain visible until either a live or retired lease conflicts. */
static void test_preserve_and_active(void)
{
    Fixture f;
    struct socket a, b, c;
    fixture_init(&f);
    socket_init(&a, &f, 41000);
    socket_init(&b, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    g_assert_cmpuint(ntohs(a.so_fport), ==, 41000);
    f.now_ns = INT64_C(3600) * 1000000000;
    g_assert_true(slirp_hostfwd_port_acquire(&b));
    g_assert_cmpuint(ntohs(b.so_fport), !=, 41000);
    /* Another AP/STA target conservatively shares the same reservation pool. */
    socket_init(&c, &f, 41000);
    c.so_laddr.s_addr = htonl(0xc0a82b01);
    g_assert_true(slirp_hostfwd_port_acquire(&c));
    g_assert_cmpuint(ntohs(c.so_fport), !=, 41000);
    g_assert_cmpuint(ntohs(c.so_fport), !=, ntohs(b.so_fport));
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&b);
    slirp_hostfwd_port_release(&c);
    slirp_hostfwd_ports_cleanup(&f.slirp);
}

/* A close does not free its port before the guest's quiet interval has elapsed. */
static void test_quiet_boundary(void)
{
    Fixture f;
    struct socket a, b, c;
    fixture_init(&f);
    socket_init(&a, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    f.now_ns = 999999;
    slirp_hostfwd_port_release(&a);
    f.now_ns = INT64_C(240000) * 1000000;
    socket_init(&b, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&b));
    g_assert_cmpuint(ntohs(b.so_fport), !=, 41000);
    f.now_ns = INT64_C(240001) * 1000000;
    socket_init(&c, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&c));
    g_assert_cmpuint(ntohs(c.so_fport), ==, 41000);
    slirp_hostfwd_port_release(&b);
    slirp_hostfwd_port_release(&c);
    slirp_hostfwd_ports_cleanup(&f.slirp);
}

/* An unleased ordinary TCP tuple must not be overwritten by an incoming alias. */
static void test_existing_tuple(void)
{
    Fixture f;
    struct socket ordinary, incoming;
    fixture_init(&f);
    socket_init(&ordinary, &f, 41000);
    ordinary.so_next = ordinary.so_prev = &f.slirp.tcb;
    f.slirp.tcb.so_next = f.slirp.tcb.so_prev = &ordinary;
    socket_init(&incoming, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&incoming));
    g_assert_cmpuint(ntohs(incoming.so_fport), !=, 41000);
    g_assert_cmpuint(ntohs(ordinary.so_fport), ==, 41000);
    slirp_hostfwd_port_release(&incoming);
    slirp_hostfwd_ports_cleanup(&f.slirp);
}

/* IPv6 translation changes its port without damaging either IPv6 address. */
static void test_ipv6(void)
{
    Fixture f;
    struct socket a, b;
    fixture_init(&f);
    socket_init(&a, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    socket_init(&b, &f, 41000);
    b.so_ffamily = b.so_lfamily = AF_INET6;
    memset(&b.so_faddr6, 0x52, sizeof(b.so_faddr6));
    memset(&b.so_laddr6, 0x73, sizeof(b.so_laddr6));
    g_assert_true(slirp_hostfwd_port_acquire(&b));
    g_assert_cmpuint(ntohs(b.so_fport6), !=, 41000);
    for (unsigned i = 0; i < sizeof(b.so_faddr6); i++) {
        g_assert_cmpuint(((uint8_t *)&b.so_faddr6)[i], ==, 0x52);
        g_assert_cmpuint(((uint8_t *)&b.so_laddr6)[i], ==, 0x73);
    }
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&b);
    slirp_hostfwd_ports_cleanup(&f.slirp);
}

/* Exhaustion fails without stealing a port; an expired lease becomes available. */
static void test_exhaustion(void)
{
    Fixture f;
    struct socket *sockets = g_new0(struct socket, SLIRP_HOSTFWD_ALIAS_COUNT + 1);
    bool seen[65536] = { false };
    fixture_init(&f);
    for (unsigned i = 0; i < SLIRP_HOSTFWD_ALIAS_COUNT; i++) {
        socket_init(&sockets[i], &f, 50000);
        g_assert_true(slirp_hostfwd_port_acquire(&sockets[i]));
        uint16_t port = ntohs(sockets[i].so_fport);
        g_assert_false(seen[port]);
        seen[port] = true;
    }
    struct socket *last = &sockets[SLIRP_HOSTFWD_ALIAS_COUNT];
    socket_init(last, &f, 50000);
    g_assert_false(slirp_hostfwd_port_acquire(last));
    g_assert_cmpuint(last->hostfwd_guest_port, ==, 0);
    g_assert_cmpuint(g_hash_table_size(f.slirp.hostfwd_port_leases), ==, SLIRP_HOSTFWD_ALIAS_COUNT);
    slirp_hostfwd_port_release(&sockets[0]);
    g_assert_false(slirp_hostfwd_port_acquire(last));
    f.now_ns = INT64_C(240001) * 1000000;
    g_assert_true(slirp_hostfwd_port_acquire(last));
    g_assert_cmpuint(ntohs(last->so_fport), ==, 50000);
    for (unsigned i = 0; i <= SLIRP_HOSTFWD_ALIAS_COUNT; i++) {
        slirp_hostfwd_port_release(&sockets[i]);
    }
    slirp_hostfwd_ports_cleanup(&f.slirp);
    g_free(sockets);
}

/* Remaining lifetimes survive migration to a different virtual clock origin. */
static void test_migration_pool(void)
{
    Fixture from, to;
    struct socket retired, active, probe, later;
    fixture_init(&from);
    fixture_init(&to);
    socket_init(&retired, &from, 41001);
    socket_init(&active, &from, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&retired));
    g_assert_true(slirp_hostfwd_port_acquire(&active));
    slirp_hostfwd_port_release(&retired);
    from.now_ns = INT64_C(1000) * 1000000;
    GByteArray *bytes = save_pool(&from);
    to.now_ns = INT64_C(5) * 1000000;
    Input input = { bytes->data, bytes->len, 0 };
    g_assert_cmpint(slirp_hostfwd_ports_load(&to.slirp, read_bytes, &input), ==, 0);
    g_assert_cmpuint(input.offset, ==, input.size);
    socket_init(&probe, &to, 41001);
    g_assert_true(slirp_hostfwd_port_acquire(&probe));
    g_assert_cmpuint(ntohs(probe.so_fport), !=, 41001);
    to.now_ns = INT64_C(239006) * 1000000;
    socket_init(&later, &to, 41001);
    g_assert_true(slirp_hostfwd_port_acquire(&later));
    g_assert_cmpuint(ntohs(later.so_fport), ==, 41001);
    slirp_hostfwd_port_release(&probe);
    socket_init(&probe, &to, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&probe));
    g_assert_cmpuint(ntohs(probe.so_fport), !=, 41000);
    slirp_hostfwd_port_release(&probe);
    slirp_hostfwd_port_release(&later);
    slirp_hostfwd_port_release(&active);
    slirp_hostfwd_ports_cleanup(&from.slirp);
    slirp_hostfwd_ports_cleanup(&to.slirp);
    g_byte_array_unref(bytes);
}

/* Failed or truncated state cannot replace an existing destination lease. */
static void test_migration_rejections(void)
{
    Fixture from, to;
    struct socket a, b, existing;
    fixture_init(&from);
    fixture_init(&to);
    socket_init(&a, &from, 41000);
    socket_init(&b, &from, 41001);
    socket_init(&existing, &to, 42000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    g_assert_true(slirp_hostfwd_port_acquire(&b));
    g_assert_true(slirp_hostfwd_port_acquire(&existing));
    GByteArray *bytes = save_pool(&from), *before = save_pool(&to);
    for (size_t length = 0; length < bytes->len; length++) {
        Input input = { bytes->data, length, 0 };
        g_assert_cmpint(slirp_hostfwd_ports_load(&to.slirp, read_bytes, &input), ==, -EINVAL);
        GByteArray *after = save_pool(&to);
        g_assert_cmpmem(before->data, before->len, after->data, after->len);
        g_byte_array_unref(after);
    }
    const unsigned positions[] = { 0, 4, 8, 10, 12, 14, 15, 18 };
    for (unsigned index = 0; index < G_N_ELEMENTS(positions); index++) {
        GByteArray *bad = g_byte_array_new_take(g_memdup2(bytes->data, bytes->len), bytes->len);
        unsigned offset = positions[index];
        if (offset == 18) { /* duplicate the preceding port */
            memcpy(bad->data + 18, bad->data + 12, 2);
        } else if (offset == 12 || offset == 14 || offset == 8) {
            memset(bad->data + offset, 0, offset == 14 ? 4 : 2);
        } else {
            bad->data[offset] = 0xff;
        }
        Input input = { bad->data, bad->len, 0 };
        g_assert_cmpint(slirp_hostfwd_ports_load(&to.slirp, read_bytes, &input), ==, -EINVAL);
        GByteArray *after = save_pool(&to);
        g_assert_cmpmem(before->data, before->len, after->data, after->len);
        g_byte_array_unref(after);
        g_byte_array_unref(bad);
    }
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&b);
    slirp_hostfwd_port_release(&existing);
    slirp_hostfwd_ports_cleanup(&from.slirp);
    slirp_hostfwd_ports_cleanup(&to.slirp);
    g_byte_array_unref(bytes);
    g_byte_array_unref(before);
}

/* A load into a live stack must not steal a port from its current host socket. */
static void test_migration_live_merge(void)
{
    Fixture from, to;
    struct socket a, existing, probe;
    fixture_init(&from);
    fixture_init(&to);
    socket_init(&a, &from, 41000);
    socket_init(&existing, &to, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    g_assert_true(slirp_hostfwd_port_acquire(&existing));
    GByteArray *bytes = save_pool(&from);
    Input input = { bytes->data, bytes->len, 0 };
    g_assert_cmpint(slirp_hostfwd_ports_load(&to.slirp, read_bytes, &input), ==, 0);
    to.now_ns = INT64_C(3600) * 1000000000;
    socket_init(&probe, &to, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&probe));
    g_assert_cmpuint(ntohs(probe.so_fport), !=, 41000);
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&existing);
    slirp_hostfwd_port_release(&probe);
    slirp_hostfwd_ports_cleanup(&from.slirp);
    slirp_hostfwd_ports_cleanup(&to.slirp);
    g_byte_array_unref(bytes);
}

/* Simulate a failed migration destination without changing any live lease. */
static slirp_ssize_t fail_write(const void *data, size_t size, void *opaque)
{
    (void)data;
    (void)size;
    (void)opaque;
    return -1;
}

/* A failed save must report its error and keep the source pool protected. */
static void test_failed_save(void)
{
    Fixture f;
    struct socket a, probe;
    fixture_init(&f);
    socket_init(&a, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    g_assert_cmpint(slirp_hostfwd_ports_save(&f.slirp, fail_write, NULL), ==, -EIO);
    socket_init(&probe, &f, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&probe));
    g_assert_cmpuint(ntohs(probe.so_fport), !=, 41000);
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&probe);
    slirp_hostfwd_ports_cleanup(&f.slirp);
}

#ifdef HOSTFWD_FULL_STATE_TEST
/* Exercise the public v5 save/load path and continued reading of legacy v4. */
static void test_full_state(void)
{
    Fixture from, to, legacy;
    struct socket a, probe;
    fixture_init(&from);
    fixture_init(&to);
    fixture_init(&legacy);
    from.slirp.ip_id = 0x1234;
    socket_init(&a, &from, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&a));
    GByteArray *bytes = g_byte_array_new();
    g_assert_cmpint(slirp_state_version(), ==, 5);
    g_assert_cmpint(slirp_state_save(&from.slirp, write_bytes, bytes), ==, 0);
    Input input = { bytes->data, bytes->len, 0 };
    g_assert_cmpint(slirp_state_load(&to.slirp, 6, read_bytes, &input), ==, -EINVAL);
    g_assert_cmpuint(input.offset, ==, 0);
    g_assert_cmpint(slirp_state_load(&to.slirp, 5, read_bytes, &input), ==, 0);
    g_assert_cmpuint(input.offset, ==, input.size);
    g_assert_cmpuint(to.slirp.ip_id, ==, from.slirp.ip_id);
    socket_init(&probe, &to, 41000);
    g_assert_true(slirp_hostfwd_port_acquire(&probe));
    g_assert_cmpuint(ntohs(probe.so_fport), !=, 41000);
    /* The v4 prefix is unchanged and did not contain a lease extension. */
    input = (Input){ bytes->data, bytes->len - 18, 0 };
    g_assert_cmpint(slirp_state_load(&legacy.slirp, 4, read_bytes, &input), ==, 0);
    g_assert_cmpuint(input.offset, ==, input.size);
    g_assert_cmpuint(legacy.slirp.ip_id, ==, from.slirp.ip_id);
    slirp_hostfwd_port_release(&a);
    slirp_hostfwd_port_release(&probe);
    slirp_hostfwd_ports_cleanup(&from.slirp);
    slirp_hostfwd_ports_cleanup(&to.slirp);
    slirp_hostfwd_ports_cleanup(&legacy.slirp);
    g_byte_array_unref(bytes);
}
#endif

/* Register behavioral boundaries, exhaustion and migration corruption controls. */
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/hostfwd/preserve-active", test_preserve_and_active);
    g_test_add_func("/hostfwd/quiet-boundary", test_quiet_boundary);
    g_test_add_func("/hostfwd/existing-tuple", test_existing_tuple);
    g_test_add_func("/hostfwd/ipv6", test_ipv6);
    g_test_add_func("/hostfwd/exhaustion", test_exhaustion);
    g_test_add_func("/hostfwd/migrate-pool", test_migration_pool);
    g_test_add_func("/hostfwd/migrate-rejections", test_migration_rejections);
    g_test_add_func("/hostfwd/migrate-live", test_migration_live_merge);
    g_test_add_func("/hostfwd/save-failure", test_failed_save);
#ifdef HOSTFWD_FULL_STATE_TEST
    g_test_add_func("/hostfwd/migrate-public-api", test_full_state);
#endif
    return g_test_run();
}
