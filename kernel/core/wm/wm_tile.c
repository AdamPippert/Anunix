/*
 * wm_tile.c — Dwindle tiling layout (modelled on Hyprland's "dwindle").
 *
 * Each workspace keeps a binary tree. Leaves are windows; inner nodes split
 * their box in two. A new window splits the box of the window it is added
 * next to and takes the second half. Whether a split is side by side or
 * stacked is decided from the box's shape at every layout -- wider than tall
 * means side by side -- as Hyprland does with preserve_split off, so the
 * layout stays sensible when gaps or the screen change.
 *
 * This file is pure geometry: no framebuffer, no surfaces, so the host
 * tests can exercise it directly.
 */

#include <anx/wm.h>
#include <anx/string.h>

struct anx_wm_tiling_config anx_wm_tiling = {
	.enabled     = true,
	.gaps_in     = 5,
	.gaps_out    = 10,
	.border_w    = 2,
	.split_ratio = 500,
	.resize_step = 50,
};

#define NIL	((int16_t)-1)

static bool oid_same(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

void anx_wm_tile_init(struct anx_wm_tile_tree *t)
{
	anx_memset(t, 0, sizeof(*t));
	t->root = NIL;
}

static int16_t find_leaf(const struct anx_wm_tile_tree *t,
			 const anx_oid_t *oid)
{
	int16_t i;

	if (!oid)
		return NIL;
	for (i = 0; i < (int16_t)ANX_WM_TILE_NODES; i++)
		if (t->nodes[i].used && t->nodes[i].is_leaf &&
		    oid_same(&t->nodes[i].leaf, oid))
			return i;
	return NIL;
}

static int16_t alloc_node(struct anx_wm_tile_tree *t)
{
	int16_t i;

	for (i = 0; i < (int16_t)ANX_WM_TILE_NODES; i++) {
		if (!t->nodes[i].used) {
			anx_memset(&t->nodes[i], 0, sizeof(t->nodes[i]));
			t->nodes[i].used = true;
			t->nodes[i].parent = NIL;
			t->nodes[i].child[0] = NIL;
			t->nodes[i].child[1] = NIL;
			return i;
		}
	}
	return NIL;
}

bool anx_wm_tile_contains(const struct anx_wm_tile_tree *t,
			  const anx_oid_t *oid)
{
	return t && find_leaf(t, oid) != NIL;
}

/* The deepest last-child leaf: the window added most recently in dwindle. */
static int16_t last_leaf(const struct anx_wm_tile_tree *t)
{
	int16_t n = t->count ? t->root : NIL;

	while (n != NIL && !t->nodes[n].is_leaf)
		n = t->nodes[n].child[1];
	return n;
}

static int16_t first_leaf(const struct anx_wm_tile_tree *t, int16_t n)
{
	while (n != NIL && !t->nodes[n].is_leaf)
		n = t->nodes[n].child[0];
	return n;
}

int anx_wm_tile_insert(struct anx_wm_tile_tree *t, const anx_oid_t *oid,
		       const anx_oid_t *target)
{
	int16_t tgt, leaf, split, parent;

	if (!t || !oid)
		return ANX_EINVAL;
	if (find_leaf(t, oid) != NIL)
		return ANX_EEXIST;

	leaf = alloc_node(t);
	if (leaf == NIL)
		return ANX_ENOMEM;
	t->nodes[leaf].is_leaf = true;
	t->nodes[leaf].leaf = *oid;

	/* count, not root, says empty: a zeroed tree has root 0. */
	if (t->count == 0) {
		t->root = leaf;
		t->count = 1;
		return ANX_OK;
	}

	tgt = find_leaf(t, target);
	if (tgt == NIL)
		tgt = last_leaf(t);

	split = alloc_node(t);
	if (split == NIL) {
		t->nodes[leaf].used = false;
		return ANX_ENOMEM;
	}

	/* The split takes the target's place; target and newcomer hang off it. */
	parent = t->nodes[tgt].parent;
	t->nodes[split].parent = parent;
	t->nodes[split].ratio = (uint16_t)anx_wm_tiling.split_ratio;
	t->nodes[split].box = t->nodes[tgt].box;
	if (parent == NIL)
		t->root = split;
	else if (t->nodes[parent].child[0] == tgt)
		t->nodes[parent].child[0] = split;
	else
		t->nodes[parent].child[1] = split;

	t->nodes[split].child[0] = tgt;
	t->nodes[split].child[1] = leaf;
	t->nodes[tgt].parent = split;
	t->nodes[leaf].parent = split;
	t->count++;
	return ANX_OK;
}

int anx_wm_tile_remove(struct anx_wm_tile_tree *t, const anx_oid_t *oid,
		       anx_oid_t *heir)
{
	int16_t leaf, parent, sib, grand, h;

	if (heir)
		heir->hi = heir->lo = 0;
	if (!t || !oid)
		return ANX_EINVAL;
	leaf = find_leaf(t, oid);
	if (leaf == NIL)
		return ANX_ENOENT;

	parent = t->nodes[leaf].parent;
	t->nodes[leaf].used = false;
	t->count--;

	if (parent == NIL) {
		t->root = NIL;
		return ANX_OK;
	}

	/* The sibling moves up into the parent's place. */
	sib = t->nodes[parent].child[0] == leaf ? t->nodes[parent].child[1]
						: t->nodes[parent].child[0];
	grand = t->nodes[parent].parent;
	t->nodes[sib].parent = grand;
	if (grand == NIL)
		t->root = sib;
	else if (t->nodes[grand].child[0] == parent)
		t->nodes[grand].child[0] = sib;
	else
		t->nodes[grand].child[1] = sib;
	t->nodes[parent].used = false;

	h = first_leaf(t, sib);
	if (heir && h != NIL)
		*heir = t->nodes[h].leaf;
	return ANX_OK;
}

int anx_wm_tile_swap(struct anx_wm_tile_tree *t, const anx_oid_t *a,
		     const anx_oid_t *b)
{
	int16_t la, lb;
	anx_oid_t tmp;

	if (!t || !a || !b)
		return ANX_EINVAL;
	la = find_leaf(t, a);
	lb = find_leaf(t, b);
	if (la == NIL || lb == NIL)
		return ANX_ENOENT;
	tmp = t->nodes[la].leaf;
	t->nodes[la].leaf = t->nodes[lb].leaf;
	t->nodes[lb].leaf = tmp;
	return ANX_OK;
}

int anx_wm_tile_resize(struct anx_wm_tile_tree *t, const anx_oid_t *oid,
		       bool vertical, int32_t delta_permille)
{
	int16_t n, p;
	int32_t r;

	if (!t)
		return ANX_EINVAL;
	n = find_leaf(t, oid);
	if (n == NIL)
		return ANX_ENOENT;

	/* Walk up to the nearest split that divides along the requested axis. */
	for (p = t->nodes[n].parent; p != NIL;
	     n = p, p = t->nodes[p].parent) {
		if (t->nodes[p].vertical != vertical)
			continue;
		r = (int32_t)t->nodes[p].ratio;
		/* Growing the first child raises its share; the second, lowers it. */
		r += t->nodes[p].child[0] == n ? delta_permille : -delta_permille;
		if (r < 100)
			r = 100;
		if (r > 900)
			r = 900;
		t->nodes[p].ratio = (uint16_t)r;
		return ANX_OK;
	}
	return ANX_ENOENT;
}

static void layout_node(struct anx_wm_tile_tree *t, int16_t n,
			struct anx_wm_rect box, uint32_t gap,
			anx_oid_t *oids, struct anx_wm_rect *boxes,
			uint32_t max, uint32_t *count)
{
	struct anx_wm_tile_node *node = &t->nodes[n];
	struct anx_wm_rect a = box, b = box;
	uint32_t span, first;

	node->box = box;
	if (node->is_leaf) {
		if (*count < max) {
			oids[*count] = node->leaf;
			boxes[*count] = box;
		}
		(*count)++;
		return;
	}

	/* Hyprland: split side by side when the box is wider than tall. */
	node->vertical = box.h > box.w;
	span = node->vertical ? box.h : box.w;
	span = span > gap ? span - gap : 0;
	first = (uint32_t)((uint64_t)span * node->ratio / 1000u);
	if (node->vertical) {
		a.h = first;
		b.y = box.y + (int32_t)(first + gap);
		b.h = span - first;
	} else {
		a.w = first;
		b.x = box.x + (int32_t)(first + gap);
		b.w = span - first;
	}
	layout_node(t, node->child[0], a, gap, oids, boxes, max, count);
	layout_node(t, node->child[1], b, gap, oids, boxes, max, count);
}

uint32_t anx_wm_tile_layout(struct anx_wm_tile_tree *t,
			    const struct anx_wm_rect *area,
			    const struct anx_wm_tiling_config *cfg,
			    anx_oid_t *oids, struct anx_wm_rect *boxes,
			    uint32_t max)
{
	struct anx_wm_rect box;
	uint32_t count = 0, out;

	if (!t || !area || !cfg || t->count == 0 || t->root == NIL)
		return 0;

	out = cfg->gaps_out;
	box = *area;
	if (box.w > 2 * out && box.h > 2 * out) {
		box.x += (int32_t)out;
		box.y += (int32_t)out;
		box.w -= 2 * out;
		box.h -= 2 * out;
	}
	/* Each window keeps gaps_in on every side, so neighbours sit twice that apart. */
	layout_node(t, t->root, box, 2 * cfg->gaps_in, oids, boxes, max,
		    &count);
	return count < max ? count : max;
}
