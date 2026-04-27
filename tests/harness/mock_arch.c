/*
 * mock_arch.c — Mock architecture functions for host-native testing.
 *
 * Stubs the arch.h interface so kernel code can run on the host
 * without actual hardware initialization.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/irq.h>
#include <anx/pci.h>
#include <anx/acpi.h>
#include <anx/credential.h>
#include <anx/model_client.h>
#include <anx/gui.h>
#include <anx/jpeg.h>
#include <anx/virtio_net.h>
#include <anx/net.h>
#include <anx/e1000.h>
#include <anx/xdna.h>
#include <anx/http.h>
#include <anx/page.h>
#include <anx/fb.h>
#include <anx/hwprobe.h>
#include <anx/driver_table.h>
#include <anx/dt.h>
#include <anx/mt7925.h>

static uint64_t mock_time = 1000000000ULL;	/* 1 second in ns */

/* 8 MiB static heap for test builds */
#define MOCK_HEAP_SIZE	(8 * 1024 * 1024)
static uint8_t mock_heap[MOCK_HEAP_SIZE]
	__attribute__((aligned(4096)));

void arch_early_init(void)
{
}

void arch_init(void)
{
	uintptr_t start = (uintptr_t)mock_heap;
	uintptr_t end = start + MOCK_HEAP_SIZE;
	anx_page_init(start, end);
}

void arch_halt(void)
{
	/* Declared noreturn — spin forever in test builds */
	for (;;)
		;
}

bool arch_irq_disable(void)
{
	return false;
}

void arch_irq_enable(void)
{
}

void arch_irq_restore(bool flags)
{
	(void)flags;
}

anx_time_t arch_time_now(void)
{
	/* Monotonically increasing mock time */
	mock_time += 1000000;	/* +1ms per call */
	return mock_time;
}

/*
 * For host-native test builds, output to stdout via raw syscall
 * so we don't need <stdio.h> in freestanding-compatible headers.
 */
extern long write(int fd, const void *buf, unsigned long count);

void arch_console_putc(char c)
{
	write(1, &c, 1);
}

void arch_console_puts(const char *s)
{
	while (*s) {
		write(1, s, 1);
		s++;
	}
}

int arch_console_getc(void)
{
	/* In test builds, return EOF-like value — no interactive input */
	return -1;
}

bool arch_console_has_input(void)
{
	return false;
}

void arch_exception_init(void)
{
}

uint64_t arch_timer_ticks(void)
{
	return 0;
}

void arch_set_timer_callback(void (*fn)(void))
{
	(void)fn; /* no-op in test harness */
}

void arch_probe_hw(struct anx_hw_inventory *inv)
{
	/* Mock: 4 CPUs, 16 GiB RAM, 1 GPU */
	inv->cpu_count = 4;
	inv->ram_bytes = 16ULL * 1024 * 1024 * 1024;
	inv->accel_count = 1;
	inv->accels[0].type = ANX_ACCEL_GPU;
	inv->accels[0].mem_bytes = 8ULL * 1024 * 1024 * 1024;
	inv->accels[0].compute_units = 32;
}

void arch_fb_detect(struct anx_fb_info *info)
{
	/* No framebuffer in test builds — tests set it up directly */
	info->available = false;
}

void arch_mb(void)
{
}

void arch_rmb(void)
{
}

void arch_wmb(void)
{
}

const char *arch_boot_cmdline(void)
{
	return NULL;
}

/*
 * Mock stubs for hardware-dependent subsystems that the real
 * driver code (compiled from DRIVER_C_ALL) calls into.
 * Only stub what lives in arch/ code or requires actual hardware.
 */

int anx_irq_register(uint8_t irq, anx_irq_handler_t handler, void *arg)
{
	(void)irq; (void)handler; (void)arg;
	return ANX_OK;
}

void anx_irq_unmask(uint8_t irq) { (void)irq; }
void anx_irq_mask(uint8_t irq) { (void)irq; }

/* fb.c, fbcon.c, font.c compiled from real sources — no mocks needed */

/* Mock GUI (gui.c excluded from test build) */
void anx_gui_init(void) {}
bool anx_gui_active(void) { return false; }
void anx_gui_terminal_putc(char c) { (void)c; }
void anx_gui_update_time(void) {}
void anx_gui_get_time(char *buf, uint32_t buflen)
{ if (buf && buflen >= 6) { buf[0]='0';buf[1]='0';buf[2]=':';buf[3]='0';buf[4]='0';buf[5]='\0'; } }
void anx_gui_set_tz_offset(int32_t h) { (void)h; }
void anx_gui_draw_char_scaled(uint32_t x, uint32_t y, char c,
    uint32_t fg, uint32_t bg, uint32_t scale)
{ (void)x;(void)y;(void)c;(void)fg;(void)bg;(void)scale; }
void anx_gui_draw_string_scaled(uint32_t x, uint32_t y, const char *s,
    uint32_t fg, uint32_t bg, uint32_t scale)
{ (void)x;(void)y;(void)s;(void)fg;(void)bg;(void)scale; }
void anx_gui_terminal_clear(void) {}
void anx_gui_disable(void) {}
int32_t anx_gui_get_tz_offset(void) { return 0; }

/* Mock JPEG splash data (real jpeg.c from lib/ is compiled) */
const uint8_t _splash_jpg_start[1] = {0};
const uint8_t _splash_jpg_end[1] = {0};

/* Mock model client */
void anx_model_client_init(const struct anx_model_endpoint *ep) { (void)ep; }
bool anx_model_client_ready(void) { return false; }
int anx_model_call(const struct anx_model_request *r, struct anx_model_response *resp)
{ (void)r; resp->content=NULL; resp->stop_reason=NULL; resp->content_len=0;
  resp->input_tokens=0; resp->output_tokens=0; resp->status_code=0; return ANX_EIO; }
void anx_model_response_free(struct anx_model_response *r) { (void)r; }

/* Mock ACPI */
static struct anx_acpi_info mock_acpi = { .valid = false };
int anx_acpi_init(void) { return ANX_OK; }
const struct anx_acpi_info *anx_acpi_get_info(void) { return &mock_acpi; }

/* Mock PCI — excluded from test build (hardware-dependent) */
static struct anx_list_head mock_pci_list = ANX_LIST_HEAD_INIT(mock_pci_list);
int anx_pci_init(void) { return ANX_OK; }
struct anx_pci_device *anx_pci_find_device(uint16_t v, uint16_t d)
{ (void)v; (void)d; return NULL; }
struct anx_list_head *anx_pci_device_list(void) { return &mock_pci_list; }
void anx_pci_enable_bus_master(struct anx_pci_device *d) { (void)d; }
const char *anx_pci_class_name(uint8_t c, uint8_t s) { (void)c; (void)s; return "unknown"; }
uint32_t anx_pci_config_read(uint8_t b, uint8_t s, uint8_t f, uint8_t o)
{ (void)b; (void)s; (void)f; (void)o; return 0xFFFFFFFF; }
void anx_pci_config_write(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint32_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; }

/* Mock virtio-net — excluded from test build */
int anx_virtio_net_init(void) { return ANX_ENOENT; }
int anx_virtio_net_send(const void *f, uint32_t l) { (void)f; (void)l; return ANX_EIO; }
int anx_virtio_net_poll(void (*cb)(const void *, uint32_t, void *), void *a)
{ (void)cb; (void)a; return 0; }
void anx_virtio_net_mac(uint8_t m[6]) { int i; for(i=0;i<6;i++) m[i]=0; }
bool anx_virtio_net_ready(void) { return false; }

/* Mock network stack — excluded from test build */
void anx_net_stack_init(const struct anx_net_config *c) { (void)c; }
void anx_net_poll(void) {}
int anx_httpd_init(uint16_t p) { (void)p; return 0; }
void anx_httpd_poll(void) {}
int anx_sshd_init(uint16_t p) { (void)p; return 0; }
void anx_sshd_poll(void) {}
void anx_eth_recv(const void *f, uint32_t l) { (void)f; (void)l; }
int anx_eth_send(const uint8_t d[6], uint16_t e, const void *p, uint32_t l)
{ (void)d; (void)e; (void)p; (void)l; return ANX_EIO; }
void anx_arp_init(void) {}
void anx_arp_set_ip(uint32_t ip) { (void)ip; }
void anx_arp_recv(const void *d, uint32_t l) { (void)d; (void)l; }
int anx_arp_resolve(uint32_t ip, uint8_t m[6]) { (void)ip; (void)m; return ANX_ETIMEDOUT; }
void anx_ipv4_init(const struct anx_net_config *c) { (void)c; }
void anx_ipv4_recv(const void *d, uint32_t l) { (void)d; (void)l; }
int anx_ipv4_send(uint32_t dst, uint8_t p, const void *d, uint32_t l)
{ (void)dst; (void)p; (void)d; (void)l; return ANX_EIO; }
uint16_t anx_ip_checksum(const void *d, uint32_t l) { (void)d; (void)l; return 0; }
uint32_t anx_ipv4_local_ip(void) { return 0; }
uint32_t anx_ipv4_dns(void) { return 0; }
void anx_icmp_recv(const void *d, uint32_t l, uint32_t s) { (void)d; (void)l; (void)s; }
int anx_icmp_ping(uint32_t ip, uint16_t s) { (void)ip; (void)s; return ANX_EIO; }
void anx_udp_init(void) {}
void anx_udp_recv(const void *d, uint32_t l, uint32_t s) { (void)d; (void)l; (void)s; }
int anx_udp_send(uint32_t dst, uint16_t sp, uint16_t dp, const void *d, uint32_t l)
{ (void)dst; (void)sp; (void)dp; (void)d; (void)l; return ANX_EIO; }
int anx_udp_bind(uint16_t p, anx_udp_recv_fn h, void *a)
{ (void)p; (void)h; (void)a; return ANX_OK; }
void anx_udp_unbind(uint16_t p) { (void)p; }
void anx_dns_init(void) {}
int anx_dns_resolve(const char *h, uint32_t *ip) { (void)h; (void)ip; return ANX_ETIMEDOUT; }
void anx_tcp_init(void) {}
void anx_tcp_recv_segment(const void *d, uint32_t l, uint32_t s) { (void)d; (void)l; (void)s; }
void anx_tcp_tick(void) {}
int anx_tcp_connect(uint32_t dst, uint16_t p, struct anx_tcp_conn **o)
{ (void)dst; (void)p; (void)o; return ANX_ETIMEDOUT; }
int anx_tcp_send(struct anx_tcp_conn *c, const void *d, uint32_t l)
{ (void)c; (void)d; (void)l; return ANX_EIO; }
int anx_tcp_recv(struct anx_tcp_conn *c, void *b, uint32_t l, uint32_t t)
{ (void)c; (void)b; (void)l; (void)t; return ANX_EIO; }
int anx_tcp_close(struct anx_tcp_conn *c) { (void)c; return ANX_OK; }
/*
 * Mock virtio-blk: RAM-backed when test_mock_blk_init() is called,
 * otherwise reports not-ready so unrelated tests see no device.
 *
 * The RAM buffer is sized for disk_store unit tests, which need the
 * superblock + journal + index + a small data region.
 */
#include <anx/mock_blk.h>

static uint8_t *mock_blk_mem;
static uint64_t mock_blk_sectors;

void test_mock_blk_init(uint64_t sectors)
{
	static uint8_t mock_blk_pool[512 * 2048]; /* 1 MiB default */

	if (sectors == 0 || sectors > 2048)
		sectors = 2048;
	mock_blk_mem = mock_blk_pool;
	mock_blk_sectors = sectors;
	/* zero the backing store so mount sees a fresh disk */
	{
		uint64_t i;

		for (i = 0; i < sectors * 512; i++)
			mock_blk_mem[i] = 0;
	}
}

void test_mock_blk_teardown(void)
{
	mock_blk_mem = NULL;
	mock_blk_sectors = 0;
}

int anx_virtio_blk_init(void) { return ANX_ENOENT; }

int anx_blk_read(uint64_t s, uint32_t c, void *b)
{
	if (!mock_blk_mem)
		return ANX_EIO;
	if (s + c > mock_blk_sectors)
		return ANX_EIO;
	{
		uint32_t i;
		uint8_t *dst = b;
		const uint8_t *src = mock_blk_mem + s * 512;

		for (i = 0; i < c * 512; i++)
			dst[i] = src[i];
	}
	return ANX_OK;
}

int anx_blk_write(uint64_t s, uint32_t c, const void *b)
{
	if (!mock_blk_mem)
		return ANX_EIO;
	if (s + c > mock_blk_sectors)
		return ANX_EIO;
	{
		uint32_t i;
		uint8_t *dst = mock_blk_mem + s * 512;
		const uint8_t *src = b;

		for (i = 0; i < c * 512; i++)
			dst[i] = src[i];
	}
	return ANX_OK;
}

uint64_t anx_blk_capacity(void) { return mock_blk_sectors; }
bool anx_blk_ready(void) { return mock_blk_mem != NULL; }

int anx_http_get(const char *h, uint16_t p, const char *pa, struct anx_http_response *r)
{ (void)h; (void)p; (void)pa; r->status_code=0; r->body=NULL; r->body_len=0; return ANX_EIO; }
int anx_http_get_authed(const char *h, uint16_t p, const char *pa,
			 const char *eh, struct anx_http_response *r)
{ (void)h; (void)p; (void)pa; (void)eh;
  r->status_code=0; r->body=NULL; r->body_len=0; return ANX_EIO; }
int anx_http_post(const char *h, uint16_t p, const char *pa, const char *ct,
		   const void *b, uint32_t bl, struct anx_http_response *r)
{ (void)h; (void)p; (void)pa; (void)ct; (void)b; (void)bl;
  r->status_code=0; r->body=NULL; r->body_len=0; return ANX_EIO; }
int anx_http_post_authed(const char *h, uint16_t p, const char *pa,
			  const char *eh, const char *ct,
			  const void *b, uint32_t bl, struct anx_http_response *r)
{ (void)h; (void)p; (void)pa; (void)eh; (void)ct; (void)b; (void)bl;
  r->status_code=0; r->body=NULL; r->body_len=0; return ANX_EIO; }
void anx_http_response_free(struct anx_http_response *r)
{ if(r) { r->body=NULL; r->body_len=0; } }
int anx_ntp_sync(uint32_t ip) { (void)ip; return ANX_ETIMEDOUT; }

/* Mock E1000 NIC — hardware excluded from test build */
int anx_e1000_init(void) { return ANX_ENODEV; }
bool anx_e1000_ready(void) { return false; }
int anx_e1000_tx(const void *f, uint16_t l) { (void)f; (void)l; return ANX_EIO; }
void anx_e1000_poll(void) {}
const uint8_t *anx_e1000_mac(void) { static uint8_t z[6]; return z; }
void anx_e1000_info(void) {}

/* Mock XDNA NPU — hardware excluded from test build */
int anx_xdna_init(void) { return ANX_ENODEV; }
bool anx_xdna_present(void) { return false; }
bool anx_xdna_ready(void) { return false; }
int anx_xdna_load_firmware(void) { return ANX_ENODEV; }
int anx_xdna_submit(const void *in, uint32_t in_len, void *out, uint32_t out_sz,
		    uint32_t part, uint32_t flags)
{ (void)in; (void)in_len; (void)out; (void)out_sz; (void)part; (void)flags;
  return ANX_ENODEV; }
void anx_xdna_info(void) {}

/* Mock driver table — driver_table.c excluded from test build (references
 * hardware-only symbols: nvme, ahci, apple_ans).  These stubs satisfy
 * references from test_main.c / kernel_main(). */
void anx_drivers_probe(void) {}
bool anx_net_probe_ok(void) { return false; }

/* Mock device tree — architecture init provides the real implementation;
 * mock_arch.c provides it for test builds where arch_init.c is not compiled. */
bool anx_dt_has_compatible(const char *compatible)
{
	(void)compatible;
	return false;
}

/* Mock MT7925 state — used by kernel_main() post-probe WiFi connect logic */
anx_mt7925_state_t anx_mt7925_state(void) { return MT7925_STATE_DOWN; }
