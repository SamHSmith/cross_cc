/* Curated upstream supplements, reconstructed at their existing vendor/ paths.
 * Archive and selected-file hashes are pinned; our WASI shims stay tracked.
 * Include after common.h and build.c's Opts/fetch/must helpers. */
#ifndef CROSS_CC_SUPPLEMENTS_H
#define CROSS_CC_SUPPLEMENTS_H

typedef struct {
    const char *lock, *root, *name;
    const char *dirs[4];
} Supplement;

static const Supplement SUPPLEMENTS[] = {
    {"musl.lock.json", "vendor/musl", "musl", {"arch", "include", "src", "COPYRIGHT"}},
    {"vendor/wasi/dlmalloc.lock.json", "vendor/wasi", "wasi", {"dlmalloc", "libc-bottom-half", "libc-top-half", NULL}}
};

typedef struct {
    Json *pin;
    Strs paths, hashes;
    char *identity;
} SupplementInput;

static int supplement_path_ok(const Supplement *s, const char *path) {
    if (!*path || is_abs(path) || strchr(path, '\\') || strchr(path, ':')) return 0;
    if (contains(path, "//") || ends_with(path, "/")) return 0;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if ((n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break;
        part = end + 1;
    }
    for (int i = 0; i < 4 && s->dirs[i]; i++) {
        size_t n = strlen(s->dirs[i]);
        if (!strncmp(path, s->dirs[i], n) && (!path[n] || path[n] == '/')) return 1;
    }
    return 0;
}

static SupplementInput supplement_input(const Opts *o, const Supplement *s) {
    SupplementInput in = {0};
    char *lock = PJ(o->root, s->lock);
    in.pin = json_load(lock);
    const char *manifest = jstr(in.pin, "files_manifest");
    if (is_abs(manifest) || contains(manifest, "..") || strchr(manifest, '\\')) fail("unsafe supplement manifest path: %s", manifest);
    char *file = PJ(o->root, manifest);
    char *digest = sha256_file(file);
    if (strcmp(digest, jstr(in.pin, "files_manifest_sha256"))) fail("supplement manifest SHA-256 mismatch: %s", file);
    Strs lines = split_lines(read_file(file, NULL));
    for (int i = 0; i < lines.n; i++) {
        char *line = lines.v[i];
        if (!*line) continue;
        if (strlen(line) < 67 || line[64] != ' ' || line[65] != ' ') fail("invalid supplement manifest line: %s", file);
        for (int k = 0; k < 64; k++)
            if (!isxdigit((unsigned char)line[k])) fail("invalid supplement file hash: %s", file);
        char *path = line + 66;
        if (!supplement_path_ok(s, path)) fail("unsafe supplement path: %s", path);
        for (int k = 0; k < in.paths.n; k++)
            if (!strcmp(in.paths.v[k], path)) fail("duplicate supplement path: %s", path);
        strs_add(&in.paths, path);
        line[64] = 0;
        strs_add(&in.hashes, line);
    }
    if (!in.paths.n) fail("empty supplement manifest: %s", file);
    const char *prefix = jstr(in.pin, "strip_prefix");
    if (!*prefix || is_abs(prefix) || contains(prefix, "..") || strchr(prefix, '\\')) fail("unsafe supplement archive prefix: %s", prefix);
    in.identity = sfmt("cross_cc-supplements-v1\n%s\n%s\n", sha256_file(lock), digest);
    return in;
}

static char *supplement_receipt(const Opts *o, const Supplement *s) {
    return PJ(o->root, "vendor", sfmt(".resources-%s-receipt", s->name));
}

static int supplement_file_count(const char *path) {
    if (is_symlink(path)) return -1;
    if (is_file(path)) return 1;
    if (!is_dir(path)) return -1;
    Strs names = {0};
    if (!list_dir(path, &names)) return -1;
    int total = 0;
    for (int i = 0; i < names.n; i++) {
        int n = supplement_file_count(PJ(path, names.v[i]));
        if (n < 0) return -1;
        total += n;
    }
    return total;
}

static int supplement_tree_ok(const Supplement *s, const SupplementInput *in, const char *root) {
    if (is_symlink(root)) return 0;
    int count = 0;
    for (int i = 0; i < 4 && s->dirs[i]; i++) {
        int n = supplement_file_count(PJ(root, s->dirs[i]));
        if (n < 0) return 0;
        count += n;
    }
    if (count != in->paths.n) return 0;
    for (int i = 0; i < in->paths.n; i++) {
        char *file = PJ(root, in->paths.v[i]);
        if (!is_file(file) || is_symlink(file) || strcmp(sha256_file(file), in->hashes.v[i])) return 0;
    }
    return 1;
}

static int supplement_ready(const Opts *o, const Supplement *s, const SupplementInput *in) {
    char *receipt = supplement_receipt(o, s);
    return is_file(receipt) && !strcmp(read_file(receipt, NULL), in->identity) && supplement_tree_ok(s, in, PJ(o->root, s->root));
}

static void require_supplements(const Opts *o) {
    for (size_t i = 0; i < sizeof SUPPLEMENTS / sizeof SUPPLEMENTS[0]; i++) {
        SupplementInput in = supplement_input(o, &SUPPLEMENTS[i]);
        if (!supplement_ready(o, &SUPPLEMENTS[i], &in)) fail("%s supplements missing or changed; run ./build.sh resources (build.cmd on Windows)", SUPPLEMENTS[i].name);
    }
}

static void supplement_prepare(const Opts *o, const Supplement *s) {
    SupplementInput in = supplement_input(o, s);
    if (supplement_ready(o, s, &in)) {
        printf("%s supplements ready (verified cache; no network)\n", s->name);
        return;
    }
    char *root = PJ(o->root, s->root);
    if (is_symlink(root)) fail("symlink supplement root: %s; preserve it and choose a regular generated directory", root);
    char *receipt = supplement_receipt(o, s);
    int recognized = is_file(receipt) && !strcmp(read_file(receipt, NULL), in.identity);
    for (int i = 0; i < 4 && s->dirs[i]; i++) {
        char *dir = PJ(root, s->dirs[i]);
        if (is_symlink(dir)) fail("modified %s supplement: %s is a symlink; preserve it and move generated paths aside before retrying", s->name, dir);
        if (exists(dir) && !recognized) fail("unrecognized or stale %s supplements at %s; move generated paths and %s aside before retrying", s->name, root, receipt);
    }
    /* Existing recognized paths may be reused only if every selected file is
     * intact. A missing whole path can be repopulated; interior edits are never
     * silently overwritten. */
    for (int i = 0; i < in.paths.n; i++) {
        char *file = PJ(root, in.paths.v[i]);
        if (is_symlink(file) || (exists(file) && (!is_file(file) || strcmp(sha256_file(file), in.hashes.v[i])))) fail("modified %s supplement: %s; preserve it and move generated paths aside before retrying", s->name, file);
    }
    char *stage = PJ(o->root, ".cache", sfmt("supplements-%s-stage", s->name));
    mkdirs(path_dir(stage));
    if (make_dir(stage) != 0) fail("supplement stage %s exists or cannot be claimed (active/interrupted run); inspect it before moving it aside", stage);
    const char *name = jstr(in.pin, "archive");
    if (!*name || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") || !strcmp(name, "..")) fail("unsafe supplement archive name: %s", name);
    char *archive = PJ(o->root, ".cache", "downloads", name);
    fetch(jstr(in.pin, "url"), jstr(in.pin, "sha256"), archive);
    char *unpack = PJ(stage, "unpack"), *selected = PJ(stage, "selected");
    mkdirs(unpack);
    Strs cmd = {0};
    strs_add(&cmd, "tar"); strs_add(&cmd, "-xf"); strs_add(&cmd, archive);
    strs_add(&cmd, "-C"); strs_add(&cmd, unpack);
    const char *prefix = jstr(in.pin, "strip_prefix");
    /* A list file also stays below Windows' command-line length limit. */
    Buf members = {0};
    for (int i = 0; i < in.paths.n; i++) buf_addf(&members, "%s/%s\n", prefix, in.paths.v[i]);
    char *list = PJ(stage, "members.txt");
    write_file(list, buf_cstr(&members));
    strs_add(&cmd, "-T"); strs_add(&cmd, list);
    must(o, o->root, &cmd);
    for (int i = 0; i < in.paths.n; i++) {
        char *file = PJ(unpack, prefix, in.paths.v[i]);
        if (!is_file(file) || is_symlink(file) || strcmp(sha256_file(file), in.hashes.v[i])) fail("supplement source SHA-256 mismatch: %s", file);
        char *dst = PJ(selected, in.paths.v[i]);
        mkdirs(path_dir(dst));
        copy_file(dst, file);
    }
    if (!supplement_tree_ok(s, &in, selected)) fail("incomplete selected %s supplement tree", s->name);
    mkdirs(root);
    for (int i = 0; i < 4 && s->dirs[i]; i++) {
        char *dst = PJ(root, s->dirs[i]);
        if (!exists(dst)) rename_path(PJ(selected, s->dirs[i]), dst);
    }
    if (!supplement_tree_ok(s, &in, root)) fail("existing %s supplements contain missing or extra files; preserve them and move generated paths aside before retrying", s->name);
    char *part = sfmt("%s.part", receipt);
    write_file(part, in.identity);
    rename_path(part, receipt);
    remove_all(stage);
    printf("%s supplements ready (SHA-256 verified archive and selected files)\n", s->name);
}

static void supplements(const Opts *o) {
    for (size_t i = 0; i < sizeof SUPPLEMENTS / sizeof SUPPLEMENTS[0]; i++) supplement_prepare(o, &SUPPLEMENTS[i]);
}
#endif
