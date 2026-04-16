-- Anunix superrouter database schema
-- Hardware profile library and driver stub registry

CREATE TABLE IF NOT EXISTS profiles (
    profile_id      TEXT PRIMARY KEY,
    node_id         TEXT NOT NULL,
    hostname        TEXT,
    arch            TEXT NOT NULL,
    submitted_at    TEXT NOT NULL,
    profile_version INTEGER NOT NULL DEFAULT 1,
    payload         TEXT NOT NULL    -- full anx:hw-profile/v1 JSON blob
);

CREATE INDEX IF NOT EXISTS idx_profiles_node ON profiles(node_id);
CREATE INDEX IF NOT EXISTS idx_profiles_arch ON profiles(arch);

CREATE TABLE IF NOT EXISTS driver_stubs (
    stub_id          TEXT PRIMARY KEY,
    vendor_id        TEXT NOT NULL,
    device_id        TEXT NOT NULL,
    class_code       TEXT NOT NULL,
    arch             TEXT NOT NULL,
    profile_id       TEXT NOT NULL REFERENCES profiles(profile_id),
    stub_text        TEXT NOT NULL,
    validation_state TEXT NOT NULL DEFAULT 'unvalidated',
    generated_at     TEXT NOT NULL,
    generated_by     TEXT NOT NULL,
    stub_oid         TEXT,           -- OID of source object on the generating node
    download_count   INTEGER NOT NULL DEFAULT 0,
    superseded_by    TEXT REFERENCES driver_stubs(stub_id)
);

CREATE INDEX IF NOT EXISTS idx_stubs_device ON driver_stubs(vendor_id, device_id, arch);
CREATE INDEX IF NOT EXISTS idx_stubs_profile ON driver_stubs(profile_id);

CREATE TABLE IF NOT EXISTS api_tokens (
    token_hash  TEXT PRIMARY KEY,   -- SHA-256 of the raw bearer token
    node_id     TEXT NOT NULL,
    hostname    TEXT,
    created_at  TEXT NOT NULL,
    last_used   TEXT,
    revoked     INTEGER NOT NULL DEFAULT 0
);
