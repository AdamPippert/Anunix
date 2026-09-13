#include <anx/tool_namespace.h>

int anx_tool_register(const struct anx_tool_descriptor *d, struct anx_tool_ref *out)
{
	(void)d; (void)out; return ANX_ENOSYS;
}
int anx_tool_update(const struct anx_tool_ref *e, const struct anx_tool_descriptor *d, struct anx_tool_ref *out)
{
	(void)e; (void)d; (void)out; return ANX_ENOSYS;
}
int anx_tool_remove(const struct anx_tool_ref *e)
{
	(void)e; return ANX_ENOSYS;
}
int anx_tool_namespace_create(struct anx_cell *c, const struct anx_tool_grant *g,
			      uint32_t n, struct anx_tool_namespace_ref *out)
{
	(void)c; (void)g; (void)n; (void)out; return ANX_ENOSYS;
}
int anx_tool_namespace_replace(const struct anx_tool_namespace_ref *e, const struct anx_tool_grant *g,
			       uint32_t n, struct anx_tool_namespace_ref *out)
{
	(void)e; (void)g; (void)n; (void)out; return ANX_ENOSYS;
}
int anx_tool_discover(struct anx_cell *c, struct anx_tool_catalog_entry *out, uint32_t n, uint32_t *count)
{
	(void)c; (void)out; (void)n; (void)count; return ANX_ENOSYS;
}
int anx_tool_authorize_call(const struct anx_cell *c, const struct anx_external_call *call)
{
	(void)c; (void)call; return ANX_ENOSYS;
}
