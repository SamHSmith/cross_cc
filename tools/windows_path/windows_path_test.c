/* Tests for windows_path.c. Build and run:
 *   cc -std=c11 -o windows_path_test tools/windows_path/windows_path_test.c && ./windows_path_test */
#include "windows_path.c"

static int failures;
#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
#define EXPECT_STR(a, b) EXPECT(strcmp((a), (b)) == 0)

static char *make_test_dir(void) {
    /* Stay inside the working directory; never a shared SDK or system temp. */
    char *dir = sfmt(".windows-path-test-%ld", (long)(size_t)&failures % 100000 + (long)file_size(__FILE__));
    remove_all(dir);
    mkdirs(dir);
    return dir;
}

static void test_receipt_binds_object_and_link_flags(void) {
    char *dir = make_test_dir();
    char *object = PJ(dir, "Path.o"), *flags = PJ(dir, "linker-flags.txt");
    write_file(object, "plain upstream object");
    write_file(flags, "Path.o LLVMSupport.lib");
    EXPECT(!wp_verified(dir, object, flags));
    wp_receipt(dir, object, flags);
    EXPECT(wp_verified(dir, object, flags));
    write_file(object, "different object");
    EXPECT(!wp_verified(dir, object, flags));
    write_file(object, "plain upstream object");
    write_file(flags, "LLVMSupport.lib");
    EXPECT(!wp_verified(dir, object, flags));
    write_file(flags, "Path.o LLVMSupport.lib");
    EXPECT(wp_verified(dir, object, flags));
    remove(object);
    EXPECT(!wp_verified(dir, object, flags));
    remove_all(dir);
}

static void test_hash_empty_input(void) {
    char *dir = make_test_dir();
    char *file = PJ(dir, "empty");
    write_file(file, "");
    EXPECT_STR(sha256_file(file), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    write_file(file, "abc");
    EXPECT_STR(sha256_file(file), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    remove_all(dir);
}

static void test_rooted_arguments_preserve_path_semantics(void) {
    char *root = getcwd_dup();
    char *args[] = {"tools/clang++", "-Iheaders", "-isystem-after", "late", "-imsvc", "sdk", "/imsvcwin-sdk", "-I=usr/include", "-isystem", "$SYSROOT/usr/include", "-Bstatic", "-Bdynamic", "-Bsymbolic", "--sysroot=relative-sdk", "-include", "config.h", "Path.cpp"};
    int n = (int)(sizeof args / sizeof args[0]);
    char *expected[] = {PJ(root, "tools/clang++"), sfmt("-I%s", PJ(root, "headers")), "-isystem-after", PJ(root, "late"), "-imsvc", PJ(root, "sdk"), sfmt("/imsvc%s", PJ(root, "win-sdk")), "-I=usr/include", "-isystem", "$SYSROOT/usr/include", "-Bstatic", "-Bdynamic", "-Bsymbolic", sfmt("--sysroot=%s", PJ(root, "relative-sdk")), "-include", PJ(root, "config.h"), "Path.cpp"};
    wp_rooted_args(args, n, root);
    for (int i = 0; i < n; i++) EXPECT_STR(args[i], expected[i]);
}

static void test_prefix_flags(void) {
    Strs f = {0};
    wp_prefix_flags("/w/in", &f);
    EXPECT(f.n == 2);
    EXPECT_STR(f.v[0], "-ffile-prefix-map=/w/in=/llvm-project-22.1.8");
    EXPECT_STR(f.v[1], "-ffile-prefix-map=/w/in/gen=/cross_cc_config");
}

int main(void) {
    tool_name = "windows_path_test";
    test_receipt_binds_object_and_link_flags();
    test_hash_empty_input();
    test_rooted_arguments_preserve_path_semantics();
    test_prefix_flags();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("windows_path tests passed");
    return 0;
}
