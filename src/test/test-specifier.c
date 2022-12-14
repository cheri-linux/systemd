/* SPDX-License-Identifier: LGPL-2.1+ */

#include "alloc-util.h"
#include "log.h"
#include "specifier.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "tests.h"

static void test_specifier_escape_one(const char *a, const char *b) {
        _cleanup_free_ char *x = NULL;

        x = specifier_escape(a);
        assert_se(streq_ptr(x, b));
}

static void test_specifier_escape(void) {
        log_info("/* %s */", __func__);

        test_specifier_escape_one(NULL, NULL);
        test_specifier_escape_one("", "");
        test_specifier_escape_one("%", "%%");
        test_specifier_escape_one("foo bar", "foo bar");
        test_specifier_escape_one("foo%bar", "foo%%bar");
        test_specifier_escape_one("%%%%%", "%%%%%%%%%%");
}

static void test_specifier_escape_strv_one(char **a, char **b) {
        _cleanup_strv_free_ char **x = NULL;

        assert_se(specifier_escape_strv(a, &x) >= 0);
        assert_se(strv_equal(x, b));
}

static void test_specifier_escape_strv(void) {
        log_info("/* %s */", __func__);

        test_specifier_escape_strv_one(NULL, NULL);
        test_specifier_escape_strv_one(STRV_MAKE(NULL), STRV_MAKE(NULL));
        test_specifier_escape_strv_one(STRV_MAKE(""), STRV_MAKE(""));
        test_specifier_escape_strv_one(STRV_MAKE("foo"), STRV_MAKE("foo"));
        test_specifier_escape_strv_one(STRV_MAKE("%"), STRV_MAKE("%%"));
        test_specifier_escape_strv_one(STRV_MAKE("foo", "%", "foo%", "%foo", "foo%foo", "quux", "%%%"),
                                       STRV_MAKE("foo", "%%", "foo%%", "%%foo", "foo%%foo", "quux", "%%%%%%"));
}

/* Any specifier functions which don't need an argument. */
static const Specifier specifier_table[] = {
        { 'm', specifier_machine_id,      NULL },
        { 'b', specifier_boot_id,         NULL },
        { 'H', specifier_host_name,       NULL },
        { 'l', specifier_short_host_name, NULL },
        { 'v', specifier_kernel_release,  NULL },
        { 'a', specifier_architecture,    NULL },
        { 'o', specifier_os_id,           NULL },
        { 'w', specifier_os_version_id,   NULL },
        { 'B', specifier_os_build_id,     NULL },
        { 'W', specifier_os_variant_id,   NULL },

        { 'g', specifier_group_name,      NULL },
        { 'G', specifier_group_id,        NULL },
        { 'U', specifier_user_id,         NULL },
        { 'u', specifier_user_name,       NULL },
        { 'h', specifier_user_home,       NULL },

        { 'T', specifier_tmp_dir,         NULL },
        { 'V', specifier_var_tmp_dir,     NULL },
        {}
};

static void test_specifiers(void) {
        log_info("/* %s */", __func__);

        for (const Specifier *s = specifier_table; s->specifier; s++) {
                char spec[3];
                _cleanup_free_ char *resolved = NULL;

                xsprintf(spec, "%%%c", s->specifier);

                assert_se(specifier_printf(spec, specifier_table, NULL, &resolved) >= 0);

                log_info("%%%c ??? %s", s->specifier, resolved);
        }
}

int main(int argc, char *argv[]) {
        test_setup_logging(LOG_DEBUG);

        test_specifier_escape();
        test_specifier_escape_strv();
        test_specifiers();

        return 0;
}
