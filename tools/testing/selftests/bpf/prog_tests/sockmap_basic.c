// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 Cloudflare
#include <error.h>

#include "test_progs.h"
#include "test_skmsg_load_helpers.skel.h"
#include "test_sockmap_update.skel.h"
#include "test_sockmap_invalid_update.skel.h"
#include "bpf_iter_sockmap.skel.h"

#include "progs/bpf_iter_sockmap.h"

#define TCP_REPAIR		19	/* TCP sock is under repair right now */

#define TCP_REPAIR_ON		1
#define TCP_REPAIR_OFF_NO_WP	-1	/* Turn off without window probes */

static int connected_socket_v4(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(80),
		.sin_addr = { inet_addr("127.0.0.1") },
	};
	socklen_t len = sizeof(addr);
	int s, repair, err;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (CHECK_FAIL(s == -1))
		goto error;

	repair = TCP_REPAIR_ON;
	err = setsockopt(s, SOL_TCP, TCP_REPAIR, &repair, sizeof(repair));
	if (CHECK_FAIL(err))
		goto error;

	err = connect(s, (struct sockaddr *)&addr, len);
	if (CHECK_FAIL(err))
		goto error;

	repair = TCP_REPAIR_OFF_NO_WP;
	err = setsockopt(s, SOL_TCP, TCP_REPAIR, &repair, sizeof(repair));
	if (CHECK_FAIL(err))
		goto error;

	return s;
error:
	perror(__func__);
	close(s);
	return -1;
}

/* Create a map, populate it with one socket, and free the map. */
static void test_sockmap_create_update_free(enum bpf_map_type map_type)
{
	const int zero = 0;
	int s, map, err;

	s = connected_socket_v4();
	if (CHECK_FAIL(s == -1))
		return;

	map = bpf_create_map(map_type, sizeof(int), sizeof(int), 1, 0);
	if (CHECK_FAIL(map == -1)) {
		perror("bpf_create_map");
		goto out;
	}

	err = bpf_map_update_elem(map, &zero, &s, BPF_NOEXIST);
	if (CHECK_FAIL(err)) {
		perror("bpf_map_update");
		goto out;
	}

out:
	close(map);
	close(s);
}

static void test_skmsg_helpers(enum bpf_map_type map_type)
{
	struct test_skmsg_load_helpers *skel;
	int err, map, verdict;

	skel = test_skmsg_load_helpers__open_and_load();
	if (CHECK_FAIL(!skel)) {
		perror("test_skmsg_load_helpers__open_and_load");
		return;
	}

	verdict = bpf_program__fd(skel->progs.prog_msg_verdict);
	map = bpf_map__fd(skel->maps.sock_map);

	err = bpf_prog_attach(verdict, map, BPF_SK_MSG_VERDICT, 0);
	if (CHECK_FAIL(err)) {
		perror("bpf_prog_attach");
		goto out;
	}

	err = bpf_prog_detach2(verdict, map, BPF_SK_MSG_VERDICT);
	if (CHECK_FAIL(err)) {
		perror("bpf_prog_detach2");
		goto out;
	}
out:
	test_skmsg_load_helpers__destroy(skel);
}

static void test_sockmap_update(enum bpf_map_type map_type)
{
	struct bpf_prog_test_run_attr tattr;
	int err, prog, src, dst, duration = 0;
	struct test_sockmap_update *skel;
	__u64 src_cookie, dst_cookie;
	const __u32 zero = 0;
	char dummy[14] = {0};
	__s64 sk;

	sk = connected_socket_v4();
	if (CHECK(sk == -1, "connected_socket_v4", "cannot connect\n"))
		return;

	skel = test_sockmap_update__open_and_load();
	if (CHECK(!skel, "open_and_load", "cannot load skeleton\n"))
		goto close_sk;

	prog = bpf_program__fd(skel->progs.copy_sock_map);
	src = bpf_map__fd(skel->maps.src);
	if (map_type == BPF_MAP_TYPE_SOCKMAP)
		dst = bpf_map__fd(skel->maps.dst_sock_map);
	else
		dst = bpf_map__fd(skel->maps.dst_sock_hash);

	err = bpf_map_update_elem(src, &zero, &sk, BPF_NOEXIST);
	if (CHECK(err, "update_elem(src)", "errno=%u\n", errno))
		goto out;

	err = bpf_map_lookup_elem(src, &zero, &src_cookie);
	if (CHECK(err, "lookup_elem(src, cookie)", "errno=%u\n", errno))
		goto out;

	tattr = (struct bpf_prog_test_run_attr){
		.prog_fd = prog,
		.repeat = 1,
		.data_in = dummy,
		.data_size_in = sizeof(dummy),
	};

	err = bpf_prog_test_run_xattr(&tattr);
	if (CHECK_ATTR(err || !tattr.retval, "bpf_prog_test_run",
		       "errno=%u retval=%u\n", errno, tattr.retval))
		goto out;

	err = bpf_map_lookup_elem(dst, &zero, &dst_cookie);
	if (CHECK(err, "lookup_elem(dst, cookie)", "errno=%u\n", errno))
		goto out;

	CHECK(dst_cookie != src_cookie, "cookie mismatch", "%llu != %llu\n",
	      dst_cookie, src_cookie);

out:
	test_sockmap_update__destroy(skel);
close_sk:
	close(sk);
}

static void test_sockmap_invalid_update(void)
{
	struct test_sockmap_invalid_update *skel;
	int duration = 0;

	skel = test_sockmap_invalid_update__open_and_load();
	if (CHECK(skel, "open_and_load", "verifier accepted map_update\n"))
		test_sockmap_invalid_update__destroy(skel);
}

static void test_sockmap_iter(enum bpf_map_type map_type)
{
	DECLARE_LIBBPF_OPTS(bpf_iter_attach_opts, opts);
	int err, len, src_fd, iter_fd, duration = 0;
	union bpf_iter_link_info linfo = {0};
	__s64 sock_fd[SOCKMAP_MAX_ENTRIES];
	__u32 i, num_sockets, max_elems;
	struct bpf_iter_sockmap *skel;
	struct bpf_link *link;
	struct bpf_map *src;
	char buf[64];

	skel = bpf_iter_sockmap__open_and_load();
	if (CHECK(!skel, "bpf_iter_sockmap__open_and_load", "skeleton open_and_load failed\n"))
		return;

	for (i = 0; i < ARRAY_SIZE(sock_fd); i++)
		sock_fd[i] = -1;

	/* Make sure we have at least one "empty" entry to test iteration of
	 * an empty slot.
	 */
	num_sockets = ARRAY_SIZE(sock_fd) - 1;

	if (map_type == BPF_MAP_TYPE_SOCKMAP) {
		src = skel->maps.sockmap;
		max_elems = bpf_map__max_entries(src);
	} else {
		src = skel->maps.sockhash;
		max_elems = num_sockets;
	}

	src_fd = bpf_map__fd(src);

	for (i = 0; i < num_sockets; i++) {
		sock_fd[i] = connected_socket_v4();
		if (CHECK(sock_fd[i] == -1, "connected_socket_v4", "cannot connect\n"))
			goto out;

		err = bpf_map_update_elem(src_fd, &i, &sock_fd[i], BPF_NOEXIST);
		if (CHECK(err, "map_update", "failed: %s\n", strerror(errno)))
			goto out;
	}

	linfo.map.map_fd = src_fd;
	opts.link_info = &linfo;
	opts.link_info_len = sizeof(linfo);
	link = bpf_program__attach_iter(skel->progs.count_elems, &opts);
	if (CHECK(IS_ERR(link), "attach_iter", "attach_iter failed\n"))
		goto out;

	iter_fd = bpf_iter_create(bpf_link__fd(link));
	if (CHECK(iter_fd < 0, "create_iter", "create_iter failed\n"))
		goto free_link;

	/* do some tests */
	while ((len = read(iter_fd, buf, sizeof(buf))) > 0)
		;
	if (CHECK(len < 0, "read", "failed: %s\n", strerror(errno)))
		goto close_iter;

	/* test results */
	if (CHECK(skel->bss->elems != max_elems, "elems", "got %u expected %u\n",
		  skel->bss->elems, max_elems))
		goto close_iter;

	if (CHECK(skel->bss->socks != num_sockets, "socks", "got %u expected %u\n",
		  skel->bss->socks, num_sockets))
		goto close_iter;

close_iter:
	close(iter_fd);
free_link:
	bpf_link__destroy(link);
out:
	for (i = 0; i < num_sockets; i++) {
		if (sock_fd[i] >= 0)
			close(sock_fd[i]);
	}
	bpf_iter_sockmap__destroy(skel);
}

void test_sockmap_basic(void)
{
	if (test__start_subtest("sockmap create_update_free"))
		test_sockmap_create_update_free(BPF_MAP_TYPE_SOCKMAP);
	if (test__start_subtest("sockhash create_update_free"))
		test_sockmap_create_update_free(BPF_MAP_TYPE_SOCKHASH);
	if (test__start_subtest("sockmap sk_msg load helpers"))
		test_skmsg_helpers(BPF_MAP_TYPE_SOCKMAP);
	if (test__start_subtest("sockhash sk_msg load helpers"))
		test_skmsg_helpers(BPF_MAP_TYPE_SOCKHASH);
	if (test__start_subtest("sockmap update"))
		test_sockmap_update(BPF_MAP_TYPE_SOCKMAP);
	if (test__start_subtest("sockhash update"))
		test_sockmap_update(BPF_MAP_TYPE_SOCKHASH);
	if (test__start_subtest("sockmap update in unsafe context"))
		test_sockmap_invalid_update();
	if (test__start_subtest("sockmap iter"))
		test_sockmap_iter(BPF_MAP_TYPE_SOCKMAP);
	if (test__start_subtest("sockhash iter"))
		test_sockmap_iter(BPF_MAP_TYPE_SOCKHASH);
}
