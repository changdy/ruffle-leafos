-- internal/db/schema.sql
-- Non-authoritative reviewer-facing copy. The runtime schema is the
-- embedded string in internal/db/db.c. Keep these in sync by hand until
-- later codegen replaces this duplication.

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS games (
    id          INTEGER PRIMARY KEY,
    system      TEXT    NOT NULL,
    name        TEXT    NOT NULL,
    source_id   TEXT    NOT NULL,
    rom_relpath TEXT    NOT NULL,
    rom_path    TEXT    NOT NULL,
    image_root_kind TEXT CHECK (image_root_kind IN ('images','roms','source')),
    image_relpath TEXT,
    image_path  TEXT,
    last_played INTEGER,
    playtime_s  INTEGER NOT NULL DEFAULT 0,
    UNIQUE (source_id, rom_relpath)
);

CREATE INDEX IF NOT EXISTS games_rom_path_idx ON games(rom_path);
CREATE INDEX IF NOT EXISTS games_image_path_idx ON games(image_path);

CREATE TABLE IF NOT EXISTS apps (
    id                  INTEGER PRIMARY KEY,
    pak_dir             TEXT NOT NULL UNIQUE,
    name                TEXT NOT NULL,
    icon                TEXT,
    platform            TEXT,
    pak_version         TEXT,
    min_jawaka_version  TEXT,
    min_leaf_version    TEXT
);

CREATE TABLE IF NOT EXISTS favorites (
    kind       TEXT NOT NULL CHECK (kind IN ('game','app')),
    target_id  INTEGER NOT NULL,
    added_at   INTEGER NOT NULL,
    PRIMARY KEY (kind, target_id)
);

CREATE TABLE IF NOT EXISTS recents (
    kind        TEXT NOT NULL CHECK (kind IN ('game','app')),
    target_id   INTEGER NOT NULL,
    last_opened INTEGER NOT NULL,
    duration_s  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (kind, target_id)
);

CREATE VIRTUAL TABLE IF NOT EXISTS games_fts USING fts5(
    name, system, content='games', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS games_ai AFTER INSERT ON games BEGIN
    INSERT INTO games_fts(rowid, name, system)
        VALUES (new.id, new.name, new.system);
END;

CREATE TRIGGER IF NOT EXISTS games_ad AFTER DELETE ON games BEGIN
    INSERT INTO games_fts(games_fts, rowid, name, system)
        VALUES ('delete', old.id, old.name, old.system);
END;

CREATE TRIGGER IF NOT EXISTS games_au AFTER UPDATE ON games BEGIN
    INSERT INTO games_fts(games_fts, rowid, name, system)
        VALUES ('delete', old.id, old.name, old.system);
    INSERT INTO games_fts(rowid, name, system)
        VALUES (new.id, new.name, new.system);
END;

CREATE VIRTUAL TABLE IF NOT EXISTS apps_fts USING fts5(
    name, pak_dir, content='apps', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS apps_ai AFTER INSERT ON apps BEGIN
    INSERT INTO apps_fts(rowid, name, pak_dir)
        VALUES (new.id, new.name, new.pak_dir);
END;

CREATE TRIGGER IF NOT EXISTS apps_ad AFTER DELETE ON apps BEGIN
    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)
        VALUES ('delete', old.id, old.name, old.pak_dir);
END;

CREATE TRIGGER IF NOT EXISTS apps_au AFTER UPDATE ON apps BEGIN
    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)
        VALUES ('delete', old.id, old.name, old.pak_dir);
    INSERT INTO apps_fts(rowid, name, pak_dir)
        VALUES (new.id, new.name, new.pak_dir);
END;

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS system_settings (
    system     TEXT NOT NULL,
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (system, key)
);

CREATE TABLE IF NOT EXISTS game_settings (
    game_id    INTEGER NOT NULL,
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (game_id, key),
    FOREIGN KEY (game_id) REFERENCES games(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS pakrat_installs (
    store_id        TEXT PRIMARY KEY,
    version         TEXT NOT NULL,
    platform        TEXT NOT NULL,
    source_id       TEXT NOT NULL DEFAULT 'primary',
    install_path    TEXT NOT NULL,
    artifact_sha256 TEXT NOT NULL,
    installed_at    TEXT NOT NULL,
    commit_token    TEXT,
    kind            TEXT NOT NULL DEFAULT 'app' CHECK (kind IN ('app','theme'))
);

CREATE INDEX IF NOT EXISTS pakrat_installs_install_path_idx
    ON pakrat_installs(install_path);

CREATE TABLE IF NOT EXISTS pakrat_service_metadata (
    store_id        TEXT PRIMARY KEY,
    install_path    TEXT NOT NULL,
    package_id      TEXT NOT NULL,
    display_name    TEXT NOT NULL,
    has_service     INTEGER NOT NULL CHECK (has_service IN (0,1)),
    service_id      TEXT NOT NULL,
    state_root      TEXT NOT NULL,
    revoke_json     TEXT NOT NULL,
    retained_json   TEXT NOT NULL,
    validated_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS pakrat_pending_uninstalls (
    store_id        TEXT PRIMARY KEY,
    source_id       TEXT NOT NULL,
    install_path    TEXT NOT NULL,
    package_id      TEXT NOT NULL,
    display_name    TEXT NOT NULL,
    has_service     INTEGER NOT NULL CHECK (has_service IN (0,1)),
    service_id      TEXT NOT NULL,
    state_root      TEXT NOT NULL,
    revoke_json     TEXT NOT NULL,
    retained_json   TEXT NOT NULL,
    created_at      TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS library_relocation_ops (
    operation_id          TEXT PRIMARY KEY,
    state                 TEXT NOT NULL CHECK (
        state IN ('prepared','committed','reverted','finishing','finished','aborted')
    ),
    expected_generation   INTEGER NOT NULL,
    mapping_generation    INTEGER NOT NULL DEFAULT 0,
    scan_ticket_generation INTEGER NOT NULL DEFAULT 0,
    item_count            INTEGER NOT NULL,
    request_fingerprint   TEXT NOT NULL,
    updated_at            INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS library_relocation_items (
    operation_id       TEXT NOT NULL,
    ordinal            INTEGER NOT NULL,
    game_id            INTEGER NOT NULL,
    old_source_id      TEXT NOT NULL,
    old_rom_relpath    TEXT NOT NULL,
    old_image_root_kind TEXT NOT NULL,
    old_image_relpath  TEXT NOT NULL,
    old_rom_path       TEXT NOT NULL,
    old_image_path     TEXT NOT NULL,
    new_source_id      TEXT NOT NULL,
    new_rom_relpath    TEXT NOT NULL,
    new_image_root_kind TEXT NOT NULL,
    new_image_relpath  TEXT NOT NULL,
    new_rom_path       TEXT NOT NULL,
    new_image_path     TEXT NOT NULL,
    old_source_snapshot TEXT NOT NULL,
    new_source_snapshot TEXT NOT NULL,
    PRIMARY KEY (operation_id, ordinal),
    FOREIGN KEY (operation_id) REFERENCES library_relocation_ops(operation_id)
        ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS library_relocation_items_game_idx
    ON library_relocation_items(game_id);

CREATE TABLE IF NOT EXISTS hidden_roms (
    source_id   TEXT NOT NULL,
    rom_relpath TEXT NOT NULL,
    member      TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (source_id, rom_relpath, member)
);

PRAGMA user_version = 6;
