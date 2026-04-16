/*
 * anx/auth.h — Multi-key authentication system.
 *
 * Manages user accounts with password hash and SSH public key
 * authentication. Each user can have multiple keys with different
 * scopes — one key for console login, another for credential store
 * access. Agents get their own keys with restricted object scopes.
 *
 * Authentication keys map to credential binding sets that control
 * which secrets and objects a session can access.
 */

#ifndef ANX_AUTH_H
#define ANX_AUTH_H

#include <anx/types.h>

#define ANX_MAX_USERS		8
#define ANX_MAX_KEYS_PER_USER	4
#define ANX_MAX_SCOPE_ENTRIES	16

/* Key scope: what a specific auth key grants access to */
enum anx_key_scope_type {
	ANX_SCOPE_CONSOLE,	/* interactive shell access */
	ANX_SCOPE_CREDENTIALS,	/* credential store read access */
	ANX_SCOPE_OBJECTS,	/* object namespace access */
	ANX_SCOPE_ADMIN,	/* full administrative access */
};

struct anx_key_scope {
	enum anx_key_scope_type type;
	char pattern[128];	/* credential name pattern or namespace glob */
};

/* Authentication key (password hash or SSH public key) */
enum anx_auth_key_type {
	ANX_AUTH_PASSWORD,	/* SHA-256 hash of password */
	ANX_AUTH_SSH_KEY,	/* SSH public key (OpenSSH format) */
};

struct anx_auth_key {
	enum anx_auth_key_type key_type;
	char key_data[512];		/* hash or public key string */
	uint32_t key_len;
	struct anx_key_scope scopes[ANX_MAX_SCOPE_ENTRIES];
	uint32_t scope_count;
	bool active;
};

/* User account */
struct anx_user {
	char username[64];
	struct anx_auth_key keys[ANX_MAX_KEYS_PER_USER];
	uint32_t key_count;
	bool active;
};

/* Active session after login */
struct anx_session {
	char username[64];
	enum anx_key_scope_type max_scope;
	struct anx_key_scope scopes[ANX_MAX_SCOPE_ENTRIES];
	uint32_t scope_count;
	bool active;
};

/* Initialize the authentication subsystem */
void anx_auth_init(void);

/* Create a user account */
int anx_auth_create_user(const char *username);

/* Add a password to a user (hashed internally) */
int anx_auth_add_password(const char *username, const char *password,
			   enum anx_key_scope_type scope);

/* Add an SSH public key to a user */
int anx_auth_add_ssh_key(const char *username, const char *pubkey,
			  enum anx_key_scope_type scope);

/* Authenticate with password, returns session on success */
int anx_auth_login_password(const char *username, const char *password,
			     struct anx_session *session);

/* Check if a session has a specific scope */
bool anx_auth_session_has_scope(const struct anx_session *session,
				 enum anx_key_scope_type scope);

/* Get the current active session (NULL if not logged in) */
struct anx_session *anx_auth_current_session(void);

/* Logout current session */
void anx_auth_logout(void);

/* Check if any users exist (for first-boot setup) */
bool anx_auth_has_users(void);

#endif /* ANX_AUTH_H */
