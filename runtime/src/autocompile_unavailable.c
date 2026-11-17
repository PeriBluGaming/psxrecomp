/* autocompile_unavailable.c — autocompile.h for hosts that cannot spawn a
 * compiler process or load the shard it produces (Nintendo Switch / libnx:
 * no fork/exec, no dynamic loader).
 *
 * Selected by CMake in place of autocompile.c. Overlay execution on such a
 * host is: build-time static shard (GAME_OVERLAY_STATIC_C) where one covers
 * the image, otherwise the dirty-RAM interpreter. Nothing is ever compiled in
 * session, so this backend never reports itself configured, busy or able to
 * start a run. A configured overlay_autocompile_cmd is recorded as the
 * degraded reason so autocompile_status explains why overlays outside the
 * static shard stay interpreted instead of silently doing nothing. */
#include "autocompile.h"

#include <stdio.h>
#include <string.h>

static char s_degraded[320];

void autocompile_configure(const char *cmd, const char *cwd) {
    (void)cwd;
    if (cmd && cmd[0] && !s_degraded[0]) {
        snprintf(s_degraded, sizeof(s_degraded), "%s",
                 "overlay autocompile is unavailable on this platform (no "
                 "process spawning or dynamic loading): overlays outside the "
                 "build-time static shard run on the dirty-RAM interpreter. "
                 "Regenerate the static shard to cover them.");
    }
}

int autocompile_configured(void) { return 0; }

void autocompile_set_cache_paths(const char *cache_dir, const char *captures) {
    (void)cache_dir;
    (void)captures;   /* no compile child exists to receive them */
}

int autocompile_check_toolchain_hash(const char *toolchain_include_dir,
                                     uint32_t runtime_hash) {
    (void)toolchain_include_dir;
    (void)runtime_hash;
    return 1;         /* no staged toolchain is ever used on this host */
}

int autocompile_toolchain_available(void) { return 0; }
int autocompile_busy(void) { return 0; }
int autocompile_request(void) { return 0; }

/* No compile ever finishes, so there is never a result to apply. */
void autocompile_poll_main(void) {}

/* No child process or worker thread is ever created. */
void autocompile_shutdown(void) {}

const char *autocompile_degraded_reason(void) {
    return s_degraded[0] ? s_degraded : NULL;
}

int autocompile_status_json(char *out, int cap) {
    char degr[400];
    size_t w = 0;
    for (const char *p = s_degraded; *p && w + 2 < sizeof(degr); p++) {
        if (*p == '"' || *p == '\\') degr[w++] = '\\';
        degr[w++] = *p;
    }
    degr[w] = '\0';
    /* Same schema as autocompile.c so debug-server consumers need no
     * platform switch; every counter is zero because nothing ever runs. */
    return snprintf(out, (size_t)(cap > 0 ? cap : 0),
        "{\"degraded\":%d,\"degraded_reason\":\"%s\","
        "\"configured\":0,\"state\":\"idle\",\"runs\":0,\"fails\":0,"
        "\"rescans\":0,\"last_exit\":-1,"
        "\"consecutive_fails\":0,\"retry_ms\":0,"
        "\"shard_ok\":0,\"shard_fail\":0,\"shard_skipped\":0,"
        "\"shard_fail_total\":0,\"shard_result_seen\":0,"
        "\"publish_ready\":0,\"publish_ready_highwater\":0,"
        "\"publish_preparing\":0,\"publish_prepare_count\":0,"
        "\"publish_prepare_fail\":0,"
        "\"publish_prepare_retry\":0,\"publish_prepare_giveup\":0,"
        "\"publish_prepare_total_us\":0,"
        "\"publish_prepare_max_us\":0,"
        "\"publish_prepare_last_us\":0,"
        "\"output_tail\":\"\",\"unavailable_on_platform\":1}",
        s_degraded[0] ? 1 : 0, degr);
}
