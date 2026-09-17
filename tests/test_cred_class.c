/*
 * test_cred_class.c — RFC-0034 connectivity secrets.
 *
 * Covers the seven acceptance criteria in RFC-0034: a connectivity secret is
 * readable only by a key-authenticated principal, is not removable by the
 * ordinary revoke path, and is removable as a class only from a key session.
 * An ordinary credential must be unaffected throughout.
 */

#include <anx/types.h>
#include <anx/credential.h>
#include <anx/auth.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s\n", (msg));			\
			return -1;					\
		}							\
	} while (0)

/*
 * anx_credential_read() copies bytes and does not terminate, so terminate
 * here using the length it reports. Comparing an unterminated buffer would
 * pass or fail on whatever happened to follow it in memory.
 */
static int read_back(const char *name, char *buf, uint32_t len)
{
	uint32_t got = 0;
	int ret;

	anx_memset(buf, 0, len);
	ret = anx_credential_read(name, buf, len - 1, &got);
	if (ret == ANX_OK) {
		if (got > len - 1)
			got = len - 1;
		buf[got] = '\0';
	}
	return ret;
}

static int test_classification(void)
{
	CHECK(anx_credential_class_of("wifi-ssid") ==
	       ANX_CRED_CLASS_CONNECTIVITY, "wifi-ssid is connectivity");
	CHECK(anx_credential_class_of("wifi-pass") ==
	       ANX_CRED_CLASS_CONNECTIVITY, "wifi-pass is connectivity");
	CHECK(anx_credential_class_of("ssh-host-key") ==
	       ANX_CRED_CLASS_CONNECTIVITY, "ssh-host-key is connectivity");
	CHECK(anx_credential_class_of("ssh-identity") ==
	       ANX_CRED_CLASS_CONNECTIVITY, "ssh-identity is connectivity");
	CHECK(anx_credential_class_of("eigentunnel-kem-sk") ==
	       ANX_CRED_CLASS_CONNECTIVITY, "eigentunnel- prefix reserved");

	CHECK(anx_credential_class_of("superrouter-api-key") ==
	       ANX_CRED_CLASS_ORDINARY, "ordinary name stays ordinary");
	/* A near-miss must not be swept into the protected class. */
	CHECK(anx_credential_class_of("wifi-ssid-backup") ==
	       ANX_CRED_CLASS_ORDINARY, "exact match only, no prefix creep");
	CHECK(anx_credential_class_of("eigentunnel") ==
	       ANX_CRED_CLASS_ORDINARY, "prefix needs the trailing dash");
	CHECK(anx_credential_class_of(NULL) ==
	       ANX_CRED_CLASS_ORDINARY, "NULL is ordinary, not a crash");
	return 0;
}

static int test_read_requires_key(void)
{
	char buf[64];

	anx_credstore_init();
	anx_auth_init();

	CHECK(anx_credential_create("wifi-pass", ANX_CRED_PASSWORD,
				      "hunter2", 7) == ANX_OK,
	       "console may write a connectivity secret");
	CHECK(anx_credential_create("ordinary-key", ANX_CRED_API_KEY,
				      "abc123", 6) == ANX_OK,
	       "ordinary credential created");

	/* No session at all: fail closed. */
	anx_auth_logout();
	CHECK(read_back("wifi-pass", buf, sizeof(buf)) == ANX_EPERM,
	       "no session cannot read a connectivity secret");
	CHECK(read_back("ordinary-key", buf, sizeof(buf)) == ANX_OK,
	       "no session can still read an ordinary credential");

	/* Console session: still refused. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_CONSOLE);
	CHECK(read_back("wifi-pass", buf, sizeof(buf)) == ANX_EPERM,
	       "console session cannot read a connectivity secret");

	/* Password session: still refused. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_PASSWORD);
	CHECK(read_back("wifi-pass", buf, sizeof(buf)) == ANX_EPERM,
	       "password session cannot read a connectivity secret");
	CHECK(read_back("ordinary-key", buf, sizeof(buf)) == ANX_OK,
	       "password session reads ordinary credentials");

	/* Key session: permitted, and the value is right. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_KEY);
	CHECK(read_back("wifi-pass", buf, sizeof(buf)) == ANX_OK,
	       "key session reads a connectivity secret");
	CHECK(anx_strcmp(buf, "hunter2") == 0, "value survives the gate");
	return 0;
}

static int test_system_accessor_still_boots(void)
{
	char buf[64];
	uint32_t got = 0;

	anx_credstore_init();
	anx_auth_init();
	anx_auth_logout();

	CHECK(anx_credential_create("wifi-ssid", ANX_CRED_OPAQUE,
				      "homenet", 7) == ANX_OK, "ssid stored");

	/* Boot-time association runs with no session and must still work. */
	CHECK(anx_credential_read_system("wifi-ssid", buf, sizeof(buf),
					   &got) == ANX_OK,
	       "system accessor works with no session");
	CHECK(got == 7, "system accessor returns the length");
	return 0;
}

static int test_removal_is_gated(void)
{
	uint32_t removed = 99;
	char buf[64];

	anx_credstore_init();
	anx_auth_init();

	CHECK(anx_credential_create("wifi-pass", ANX_CRED_PASSWORD,
				      "hunter2", 7) == ANX_OK, "ssid stored");
	CHECK(anx_credential_create("ssh-identity", ANX_CRED_PRIVATE_KEY,
				      "0123456789abcdef", 16) == ANX_OK,
	       "identity stored");
	CHECK(anx_credential_create("ordinary-key", ANX_CRED_API_KEY,
				      "abc123", 6) == ANX_OK, "ordinary stored");

	/* The ordinary revoke path refuses, whatever the session. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_KEY);
	CHECK(anx_credential_revoke("wifi-pass") == ANX_EPERM,
	       "ordinary revoke refuses a connectivity secret");
	CHECK(anx_credential_exists("wifi-pass"), "and leaves it in place");

	/* Wipe refuses without a key session. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_PASSWORD);
	CHECK(anx_credential_wipe_connectivity(&removed) == ANX_EPERM,
	       "wipe refuses a password session");
	CHECK(removed == 0, "refusal reports nothing removed");
	CHECK(anx_credential_exists("wifi-pass"), "nothing was removed");

	anx_auth_logout();
	CHECK(anx_credential_wipe_connectivity(NULL) == ANX_EPERM,
	       "wipe refuses with no session, and tolerates a NULL count");
	CHECK(anx_credential_exists("wifi-pass"), "still nothing removed");

	/* Wipe succeeds from a key session and clears the whole class. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_KEY);
	CHECK(anx_credential_wipe_connectivity(&removed) == ANX_OK,
	       "wipe succeeds from a key session");
	CHECK(removed == 2, "both connectivity secrets removed");
	CHECK(!anx_credential_exists("wifi-pass"), "wifi-pass gone");
	CHECK(!anx_credential_exists("ssh-identity"), "ssh-identity gone");

	/* The ordinary credential is untouched and still readable. */
	CHECK(anx_credential_exists("ordinary-key"), "ordinary survives");
	CHECK(read_back("ordinary-key", buf, sizeof(buf)) == ANX_OK,
	       "ordinary still readable");
	CHECK(anx_strcmp(buf, "abc123") == 0, "ordinary value intact");
	return 0;
}

int test_cred_class(void)
{
	int r;

	r = test_classification();
	if (r != 0)
		return r;
	r = test_read_requires_key();
	if (r != 0)
		return r;
	r = test_system_accessor_still_boots();
	if (r != 0)
		return r;
	r = test_removal_is_gated();
	if (r != 0)
		return r;

	/* Leave no session behind for whatever test runs next. */
	anx_auth_logout();
	return 0;
}

/*
 * Loading the store once replayed every entry through
 * anx_credential_create(), whose save rewrote the buffer the load loop was
 * reading. Only the first credential came back, and the disk copy was cut
 * down to it. Store several, "reboot" twice, and expect all of them both
 * times: the second reboot proves the first load left the disk intact.
 */
int test_cred_persist(void)
{
	static const char *const names[] = {
		"ssh-host-key", "ordinary-key", "ssh-authorized-keys",
	};
	static const char *const values[] = {
		"host-secret", "abc123", "0123456789abcdef0123456789abcdef",
	};
	char buf[64];
	uint32_t i, boot;

	test_mock_blk_init(1024);
	CHECK(anx_disk_format("credtest") == ANX_OK, "format mock disk");

	anx_credstore_init();
	anx_auth_init();
	for (i = 0; i < 3; i++)
		CHECK(anx_credential_create(names[i], ANX_CRED_OPAQUE,
					      values[i],
					      (uint32_t)anx_strlen(values[i]))
		       == ANX_OK, "create before reboot");

	/* read_back() needs a key session for the connectivity names. */
	anx_auth_session_begin("adam", ANX_AUTH_BY_KEY);
	for (boot = 0; boot < 2; boot++) {
		anx_credstore_init();
		anx_credstore_load();
		for (i = 0; i < 3; i++) {
			CHECK(anx_credential_exists(names[i]),
			       "credential survives reboot");
			CHECK(read_back(names[i], buf, sizeof(buf)) == ANX_OK,
			       "reloaded credential readable");
			CHECK(anx_strcmp(buf, values[i]) == 0,
			       "reloaded value intact");
		}
	}

	/* A change after load is still persisted. */
	CHECK(anx_credential_revoke("ordinary-key") == ANX_OK,
	       "revoke after load");
	anx_credstore_init();
	anx_credstore_load();
	CHECK(!anx_credential_exists("ordinary-key"), "revoke persisted");
	CHECK(anx_credential_exists("ssh-authorized-keys"),
	       "others still present after revoke");

	anx_auth_logout();
	return 0;
}
