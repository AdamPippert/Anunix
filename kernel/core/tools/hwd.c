/*
 * hwd.c — Hardware Discovery agent (RFC-0011).
 *
 * Probes PCI and ACPI, synthesises an anx:hw-profile/v1 structured data
 * State Object, and stores it in the object store for inspection and
 * eventual upload to superrouter.
 *
 * Sub-commands:
 *   hwd scan       — probe hardware and print summary
 *   hwd profile    — scan + create hw-profile State Object
 *   hwd show       — print stored hw-profile (if any)
 *   hwd stubs      — list PCI devices that lack a known driver
 *
 * JSON payload is built with a simple append-into-buffer helper; no
 * heap allocation beyond the State Object payload buffer itself.
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/pci.h>
#include <anx/acpi.h>
#include <anx/hwprobe.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/list.h>

/* ------------------------------------------------------------------ */
/* JSON builder                                                         */
/* ------------------------------------------------------------------ */

#define HWD_JSON_CAP  4096u

struct json_buf {
	char    *data;
	uint32_t len;
	uint32_t cap;
};

static void jb_init(struct json_buf *b, char *data, uint32_t cap)
{
	b->data = data;
	b->len  = 0;
	b->cap  = cap;
}

/* Append a NUL-terminated string (truncates silently if full) */
static void jb_str(struct json_buf *b, const char *s)
{
	while (*s && b->len + 1 < b->cap)
		b->data[b->len++] = *s++;
	b->data[b->len] = '\0';
}

/* Append a 4-digit hex value (for PCI IDs) */
static void jb_hex4(struct json_buf *b, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	char tmp[6];
	tmp[0] = '0'; tmp[1] = 'x';
	tmp[2] = hex[(v >> 12) & 0xF];
	tmp[3] = hex[(v >>  8) & 0xF];
	tmp[4] = hex[(v >>  4) & 0xF];
	tmp[5] = hex[(v >>  0) & 0xF];
	tmp[6 - 1] = '\0';
	uint32_t i;
	for (i = 0; i < 6 && b->len + 1 < b->cap; i++)
		b->data[b->len++] = tmp[i];
	b->data[b->len] = '\0';
}

/* Append a quoted JSON string, escaping backslash and double-quote */
static void jb_qstr(struct json_buf *b, const char *s)
{
	b->data[b->len++] = '"';
	while (*s && b->len + 2 < b->cap) {
		if (*s == '"' || *s == '\\')
			b->data[b->len++] = '\\';
		b->data[b->len++] = *s++;
	}
	if (b->len + 1 < b->cap)
		b->data[b->len++] = '"';
	b->data[b->len] = '\0';
}

/* Append a uint64 as decimal */
static void jb_u64(struct json_buf *b, uint64_t v)
{
	/* Build digits into a small stack buffer, then reverse-append */
	char tmp[24];
	int i = 0, j;
	if (v == 0) { jb_str(b, "0"); return; }
	while (v && i < 23) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
	for (j = i - 1; j >= 0 && b->len + 1 < b->cap; j--)
		b->data[b->len++] = tmp[j];
	b->data[b->len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Driver knowledge table — known PCI class→driver name mappings       */
/* ------------------------------------------------------------------ */

struct pci_class_driver {
	uint8_t class_code;
	uint8_t subclass;
	const char *driver;
};

static const struct pci_class_driver known_drivers[] = {
	{ 0x01, 0x01, "virtio_blk" },   /* IDE storage (virtio compat) */
	{ 0x01, 0x06, "virtio_blk" },   /* SATA/AHCI */
	{ 0x02, 0x00, "virtio_net" },   /* Ethernet */
	{ 0x03, 0x00, "fb" },           /* VGA / display */
	{ 0x0C, 0x03, "usb_mouse" },    /* USB HCI */
	{ 0x00, 0x00, NULL }            /* sentinel */
};

static const char *driver_for(uint8_t class_code, uint8_t subclass)
{
	const struct pci_class_driver *d;
	for (d = known_drivers; d->driver; d++) {
		if (d->class_code == class_code && d->subclass == subclass)
			return d->driver;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* hwd scan — enumerate and display hardware                           */
/* ------------------------------------------------------------------ */

static void hwd_scan(void)
{
	const struct anx_hw_inventory *inv = anx_hwprobe_get();
	const struct anx_acpi_info    *acpi = anx_acpi_get_info();
	struct anx_list_head *list, *pos;
	uint32_t i;

	kprintf("=== Hardware Discovery Scan ===\n\n");

	/* CPU / RAM from hwprobe */
	kprintf("CPU cores : %u\n", (unsigned)inv->cpu_count);
	kprintf("RAM       : %llu MiB\n",
	        (unsigned long long)(inv->ram_bytes >> 20));

	/* Accelerators */
	if (inv->accel_count) {
		kprintf("Accelerators (%u):\n", (unsigned)inv->accel_count);
		for (i = 0; i < inv->accel_count; i++) {
			const struct anx_hw_accel *a = &inv->accels[i];
			kprintf("  [%u] %s  %u CU  %llu MiB\n",
			        i, a->name,
			        (unsigned)a->compute_units,
			        (unsigned long long)(a->mem_bytes >> 20));
		}
	}

	/* ACPI topology */
	if (acpi && acpi->valid) {
		kprintf("ACPI rev  : %u\n", (unsigned)acpi->acpi_revision);
		kprintf("LAPIC CPUs: %u\n", (unsigned)acpi->cpu_count);
		kprintf("IOAPIC    : %u  base=0x%08x\n",
		        (unsigned)acpi->ioapic_count,
		        (unsigned)acpi->ioapic_addr);
	}

	/* PCI devices */
	kprintf("\nPCI devices:\n");
	kprintf("  %-6s  %-6s  %-4s  %-4s  %-30s  %s\n",
	        "Bus", "Slot", "VID", "DID", "Class", "Driver");

	list = anx_pci_device_list();
	ANX_LIST_FOR_EACH(pos, list) {
		struct anx_pci_device *dev =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		const char *drv = driver_for(dev->class_code, dev->subclass);
		const char *cls = anx_pci_class_name(dev->class_code,
		                                      dev->subclass);

		kprintf("  %02x:%02x.%x  %04x  %04x  %-30s  %s\n",
		        dev->bus, dev->slot, dev->func,
		        dev->vendor_id, dev->device_id,
		        cls,
		        drv ? drv : "(no driver)");
	}
	kprintf("\n");
}

/* ------------------------------------------------------------------ */
/* hwd stubs — list devices without a driver                           */
/* ------------------------------------------------------------------ */

static void hwd_stubs(void)
{
	struct anx_list_head *list, *pos;
	uint32_t count = 0;

	kprintf("PCI devices without a known driver:\n");
	kprintf("  %-6s  %-6s  %-4s  %-4s  %s\n",
	        "Bus", "Slot", "VID", "DID", "Class");

	list = anx_pci_device_list();
	ANX_LIST_FOR_EACH(pos, list) {
		struct anx_pci_device *dev =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		if (driver_for(dev->class_code, dev->subclass))
			continue;
		kprintf("  %02x:%02x.%x  %04x  %04x  %s\n",
		        dev->bus, dev->slot, dev->func,
		        dev->vendor_id, dev->device_id,
		        anx_pci_class_name(dev->class_code, dev->subclass));
		count++;
	}
	if (count == 0)
		kprintf("  (none — all devices have drivers)\n");
	kprintf("\n%u device(s) need driver stubs\n", (unsigned)count);
}

/* ------------------------------------------------------------------ */
/* Stored profile OID (set by hwd profile, read by hwd show)          */
/* ------------------------------------------------------------------ */

static anx_oid_t stored_profile_oid;
static bool      have_stored_profile;

/* ------------------------------------------------------------------ */
/* hwd profile — create hw-profile/v1 State Object                    */
/* ------------------------------------------------------------------ */

static void hwd_profile(void)
{
	const struct anx_hw_inventory *inv = anx_hwprobe_get();
	const struct anx_acpi_info    *acpi = anx_acpi_get_info();
	struct anx_list_head *list, *pos;
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	char *buf;
	struct json_buf jb;
	anx_oid_t profile_oid;
	uint32_t i;
	int rc;

	buf = anx_zalloc(HWD_JSON_CAP);
	if (!buf) {
		kprintf("hwd: out of memory\n");
		return;
	}

	jb_init(&jb, buf, HWD_JSON_CAP);

	/* Build profile JSON */
	jb_str(&jb, "{");

	jb_str(&jb, "\"schema\":\"anx:hw-profile/v1\",");
	jb_str(&jb, "\"arch\":\"x86_64\",");
	jb_str(&jb, "\"kernel_version\":\"");
	jb_str(&jb, "2026.4.17");
	jb_str(&jb, "\",");

	/* CPU */
	jb_str(&jb, "\"cpu\":{");
	jb_str(&jb, "\"count\":");
	jb_u64(&jb, inv->cpu_count);
	if (acpi && acpi->valid) {
		jb_str(&jb, ",\"acpi_lapic_count\":");
		jb_u64(&jb, acpi->cpu_count);
	}
	jb_str(&jb, "},");

	/* RAM */
	jb_str(&jb, "\"ram_bytes\":");
	jb_u64(&jb, inv->ram_bytes);
	jb_str(&jb, ",");

	/* Accelerators */
	jb_str(&jb, "\"accelerators\":[");
	for (i = 0; i < inv->accel_count; i++) {
		const struct anx_hw_accel *a = &inv->accels[i];
		if (i) jb_str(&jb, ",");
		jb_str(&jb, "{\"name\":");
		jb_qstr(&jb, a->name);
		jb_str(&jb, ",\"compute_units\":");
		jb_u64(&jb, a->compute_units);
		jb_str(&jb, ",\"mem_bytes\":");
		jb_u64(&jb, a->mem_bytes);
		jb_str(&jb, "}");
	}
	jb_str(&jb, "],");

	/* PCI devices */
	jb_str(&jb, "\"pci_devices\":[");
	list = anx_pci_device_list();
	i = 0;
	ANX_LIST_FOR_EACH(pos, list) {
		struct anx_pci_device *dev =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		const char *drv = driver_for(dev->class_code, dev->subclass);
		const char *cls = anx_pci_class_name(dev->class_code,
		                                      dev->subclass);
		if (i++) jb_str(&jb, ",");
		jb_str(&jb, "{\"vendor_id\":");
		jb_hex4(&jb, dev->vendor_id);
		jb_str(&jb, ",\"device_id\":");
		jb_hex4(&jb, dev->device_id);
		jb_str(&jb, ",\"class\":");
		jb_hex4(&jb, dev->class_code);
		jb_str(&jb, ",\"subclass\":");
		jb_hex4(&jb, dev->subclass);
		jb_str(&jb, ",\"class_name\":");
		jb_qstr(&jb, cls);
		jb_str(&jb, ",\"driver_status\":");
		jb_qstr(&jb, drv ? "known" : "needs_stub");
		jb_str(&jb, "}");
	}
	jb_str(&jb, "]");

	jb_str(&jb, "}");

	/* Create the State Object */
	anx_uuid_generate(&profile_oid);
	anx_memset(&params, 0, sizeof(params));
	params.object_type  = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri   = "anx:hw-profile/v1";
	params.schema_version = "1";
	params.payload      = buf;
	params.payload_size = (uint64_t)jb.len;

	rc = anx_so_create(&params, &obj);
	if (rc != ANX_OK) {
		kprintf("hwd: failed to create State Object (%d)\n", rc);
		anx_free(buf);
		return;
	}

	stored_profile_oid  = obj->oid;
	have_stored_profile = true;
	anx_objstore_release(obj);
	anx_free(buf);

	kprintf("hw-profile created: OID ");
	kprintf("%016llx%016llx\n",
	        (unsigned long long)stored_profile_oid.hi,
	        (unsigned long long)stored_profile_oid.lo);
	kprintf("schema: anx:hw-profile/v1  payload: %u bytes\n",
	        (unsigned)jb.len);
}

/* ------------------------------------------------------------------ */
/* hwd show — print stored profile                                     */
/* ------------------------------------------------------------------ */

static void hwd_show(void)
{
	struct anx_state_object *obj;
	char *buf;
	int rc;

	if (!have_stored_profile) {
		kprintf("hwd: no profile stored — run 'hwd profile' first\n");
		return;
	}

	obj = anx_objstore_lookup(&stored_profile_oid);
	if (!obj) {
		kprintf("hwd: profile object not found\n");
		return;
	}

	buf = anx_zalloc((uint32_t)(obj->payload_size + 1));
	if (!buf) {
		anx_objstore_release(obj);
		kprintf("hwd: out of memory\n");
		return;
	}

	{
		struct anx_object_handle h;
		rc = anx_so_open(&stored_profile_oid, ANX_OPEN_READ, &h);
		if (rc == ANX_OK) {
			anx_so_read_payload(&h, 0, buf,
			                     obj->payload_size);
			anx_so_close(&h);
		}
	}
	anx_objstore_release(obj);

	kprintf("%s\n", buf);
	anx_free(buf);
}

/* ------------------------------------------------------------------ */
/* cmd_hwd — dispatch                                                  */
/* ------------------------------------------------------------------ */

static void hwd_usage(void)
{
	kprintf("usage: hwd <subcommand>\n");
	kprintf("  scan     probe PCI/ACPI and display summary\n");
	kprintf("  profile  scan + create anx:hw-profile/v1 State Object\n");
	kprintf("  show     print stored hardware profile\n");
	kprintf("  stubs    list devices without a known driver\n");
}

void
cmd_hwd(int argc, char **argv)
{
	if (argc < 2) {
		/* Default: full discovery */
		hwd_scan();
		return;
	}

	if (anx_strcmp(argv[1], "scan") == 0) {
		hwd_scan();
	} else if (anx_strcmp(argv[1], "profile") == 0) {
		hwd_scan();
		hwd_profile();
	} else if (anx_strcmp(argv[1], "show") == 0) {
		hwd_show();
	} else if (anx_strcmp(argv[1], "stubs") == 0) {
		hwd_stubs();
	} else {
		hwd_usage();
	}
}
