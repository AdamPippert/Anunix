/*
 * test_main.c — Kernel unit test runner.
 *
 * Runs all registered test suites and reports pass/fail.
 * Compiled as a host-native binary (not freestanding).
 */

#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/arch.h>

/* Test function signature */
typedef int (*test_fn)(void);

struct test_case {
	const char *name;
	test_fn fn;
};

/* External test suites — each returns 0 on success */
int test_state_object(void);
int test_cell_lifecycle(void);
int test_cell_runtime(void);
int test_memplane(void);
int test_engine_registry(void);
int test_scheduler(void);
int test_capability(void);
int test_fb(void);
int test_engine_lifecycle(void);
int test_resource_lease(void);
int test_model_server(void);
int test_posix(void);
int test_tensor(void);
int test_tensor_codec(void);
int test_model(void);
int test_tensor_ops(void);
int test_crypto(void);
int test_sshd_crypto(void);
int test_input_routing(void);
int test_compositor_cell(void);
int test_shm_ipc(void);
int test_conformance_harness(void);
int test_userspace_prereqs(void);
int test_rlm(void);
int test_external_call(void);
int test_disk_store(void);
int test_route_planner(void);
int test_vm_object(void);
int test_workflow(void);
int test_theme(void);
int test_event_qos(void);
int test_compositor_dirty_rect(void);

static struct test_case tests[] = {
	{ "state_object",	test_state_object },
	{ "cell_lifecycle",	test_cell_lifecycle },
	{ "cell_runtime",	test_cell_runtime },
	{ "memplane",		test_memplane },
	{ "engine_registry",	test_engine_registry },
	{ "scheduler",		test_scheduler },
	{ "capability",		test_capability },
	{ "fb",			test_fb },
	{ "engine_lifecycle",	test_engine_lifecycle },
	{ "resource_lease",	test_resource_lease },
	{ "model_server",	test_model_server },
	{ "posix",		test_posix },
	{ "tensor",		test_tensor },
	{ "tensor_codec",	test_tensor_codec },
	{ "model",		test_model },
	{ "tensor_ops",		test_tensor_ops },
	{ "crypto",		test_crypto },
	{ "sshd_crypto",	test_sshd_crypto },
	{ "input_routing",	test_input_routing },
	{ "compositor_cell",	test_compositor_cell },
	{ "shm_ipc",		test_shm_ipc },
	{ "conformance_harness", test_conformance_harness },
	{ "userspace_prereqs", test_userspace_prereqs },
	{ "rlm",		test_rlm },
	{ "external_call",	test_external_call },
	{ "disk_store",		test_disk_store },
	{ "route_planner",	test_route_planner },
	{ "vm_object",		test_vm_object },
	{ "workflow",		test_workflow },
	{ "theme",		test_theme },
	{ "event_qos",		test_event_qos },
	{ "compositor_dirty_rect", test_compositor_dirty_rect },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

static int passed;
static int failed;

void kernel_main(void)
{
	uint32_t i;

	/* Initialize mock hardware (sets up page allocator) */
	arch_init();

	kprintf("=== Anunix Kernel Tests ===\n\n");

	for (i = 0; i < NUM_TESTS; i++) {
		int ret;

		kprintf("  [RUN]  %s\n", tests[i].name);
		ret = tests[i].fn();
		if (ret == 0) {
			kprintf("  [PASS] %s\n", tests[i].name);
			passed++;
		} else {
			kprintf("  [FAIL] %s (error %d)\n",
				tests[i].name, ret);
			failed++;
		}
	}

	kprintf("\n=== Results: %d passed, %d failed ===\n",
		passed, failed);
}

/* Host-native entry point */
int main(void)
{
	kernel_main();
	return failed > 0 ? 1 : 0;
}
