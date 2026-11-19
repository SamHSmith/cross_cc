/* Offline regression tests for pinned resource acquisition. Uses a tiny local
 * archive, never the network or the developer's real resource cache. */
#define main cross_cc_build_main
#include "../tools/build.c"
#undef main

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void fixture_lock(const char *root, const char *hash) {
    write_file(PJ(root, "upstream.lock.json"), sfmt(
        "{\"resources\":{\"url\":\"https://invalid.invalid/resources.tar.xz\","
        "\"sha256\":\"%s\",\"strip_prefix\":\"fixture/lib\","
        "\"extract\":[\"include\",\"libc\",\"libcxx\",\"libcxxabi\",\"libunwind\",\"libtsan\"]}}\n", hash));
}

static int prepare_child(const char *self, const char *root, const char *message) {
    char *args[] = {(char *)self, "--prepare", (char *)root, NULL};
    char *out = NULL;
    int code = run_capture(args, NULL, NULL, &out);
    if (code == 0) CHECK(out && contains(out, message));
    return code;
}

int main(int argc, char **argv) {
    tool_name = "dependency_resources_test";
    if (argc == 3) {
        Opts o = {0};
        o.root = argv[2];
        if (!strcmp(argv[1], "--build")) build(&o);
        else if (!strcmp(argv[1], "--package")) package_dist(&o);
        else if (!strcmp(argv[1], "--prepare")) resources(&o);
        else fail("unknown test action");
        return 0;
    }
    char *self = abs_path(argv[0]);
    char *root = abs_path(sfmt("build/tests/resources-fixture-%lu", (unsigned long)(size_t)&failures));
    if (exists(root)) fail("test fixture already exists: %s", root);
    char *input = PJ(root, "input");
    char *archive = PJ(root, ".cache", "downloads", "resources.tar.xz");
    mkdirs(path_dir(archive));
    for (int i = 0; RESOURCE_DIRS[i]; i++) {
        char *dir = PJ(input, "fixture", "lib", RESOURCE_DIRS[i]);
        mkdirs(dir);
        write_file(PJ(dir, "LICENSE"), sfmt("test notice %s\n", RESOURCE_DIRS[i]));
    }
    char *tar[] = {"tar", "-cJf", archive, "-C", input, "fixture", NULL};
    must_run(tar, NULL, NULL);
    char *hash = sha256_file(archive);
    fixture_lock(root, hash);
    Opts o = {0};
    o.root = root;

    /* Cold preparation verifies the preseeded archive and retains notices. */
    CHECK(prepare_child(self, root, "SHA-256 verified") == 0);
    CHECK(resources_ready(&o, resource_identity(&o)));
    for (int i = 0; RESOURCE_DIRS[i]; i++) {
        char *expected = read_file(PJ(input, "fixture", "lib", RESOURCE_DIRS[i], "LICENSE"), NULL);
        CHECK(!strcmp(read_file(PJ(root, "lib", RESOURCE_DIRS[i], "LICENSE"), NULL), expected));
    }
    CHECK(!exists(PJ(root, ".cache", "resources-stage")));

    /* An extracted cache needs neither an archive nor a downloader. */
    char *saved = PJ(root, "saved.tar.xz");
    rename_path(archive, saved);
    CHECK(prepare_child(self, root, "cached; no network") == 0);
    rename_path(saved, archive);

    /* A missing sibling is repaired from the verified cached archive. */
    remove_all(PJ(root, "lib", "libcxx"));
    CHECK(!resources_ready(&o, resource_identity(&o)));
    CHECK(prepare_child(self, root, "SHA-256 verified") == 0);
    CHECK(resources_ready(&o, resource_identity(&o)));

    /* Never extract an archive with a bad hash or bless incomplete output. */
    remove_all(PJ(root, "lib", "libunwind"));
    rename_path(archive, saved);
    write_file(archive, "corrupt archive");
    CHECK(prepare_child(self, root, "hash mismatch") != 0);
    CHECK(!is_dir(PJ(root, ".cache", "resources-stage", "libunwind")));
    CHECK(!is_dir(PJ(root, "lib", "libunwind")));
    remove_all(PJ(root, ".cache", "resources-stage"));
    remove_file(archive);
    rename_path(saved, archive);
    CHECK(prepare_child(self, root, "SHA-256 verified") == 0);

    /* A changed pin invalidates the receipt without deleting local resources. */
    fixture_lock(root, "0000000000000000000000000000000000000000000000000000000000000000");
    CHECK(!resources_ready(&o, resource_identity(&o)));
    CHECK(prepare_child(self, root, "unrecognized or stale resources") != 0);
    CHECK(is_file(PJ(root, "lib", "libc", "LICENSE")));
    fixture_lock(root, hash);

    /* Do not overwrite a tree with unknown provenance or an interrupted stage. */
    remove_file(PJ(root, "lib", ".resources-receipt"));
    CHECK(prepare_child(self, root, "unrecognized or stale resources") != 0);
    /* Direct commands bypass make; they must still fail before creating output. */
    char *build_args[] = {self, "--build", root, NULL};
    char *package_args[] = {self, "--package", root, NULL};
    CHECK(run_cmd(build_args, NULL, NULL) != 0);
    CHECK(run_cmd(package_args, NULL, NULL) != 0);
    CHECK(!exists(PJ(root, "bin")));
    CHECK(!exists(PJ(root, "dist")));
    for (int i = 0; RESOURCE_DIRS[i]; i++) remove_all(PJ(root, "lib", RESOURCE_DIRS[i]));
    mkdirs(PJ(root, ".cache", "resources-stage"));
    CHECK(prepare_child(self, root, "interrupted run") != 0);
    remove_all(root);
    if (failures) { fprintf(stderr, "%d resource test failures\n", failures); return 1; }
    puts("resource acquisition tests passed (offline)");
    return 0;
}
