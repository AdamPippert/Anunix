/* Revoking a scheduler domain contains its descendants without stopping peers. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/sched_domain.h>
#include <anx/string.h>
int anx_research_day061(void)
{
	struct anx_cell *root = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_sched_domain_view domain = {0};
	struct anx_sched_domain_spec spec = { .schema = 1, .cpu_mask = 1,
		.queue_mask = (1U << ANX_QUEUE_CLASS_COUNT) - 1, .maximum_priority = ANX_PRIO_HIGH };
	anx_strlcpy(intent.name, "research-day-061", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &root);
	if (ret != ANX_OK) return ret;
	root->execution.allow_recursive_cells = true;
	ret = -6101;
	if (anx_sched_domain_create(&root->cid, 0, &spec, &domain) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	anx_cell_cancel(root);
	if (domain.id) anx_sched_domain_destroy(domain.id);
	anx_cell_destroy(root);
	return ret;
}
#endif
