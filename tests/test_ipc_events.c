/* The `subscribe` argument, which is the only part of the event stream that
 * makes a decision worth checking. Whether an event reaches a socket needs a
 * compositor and belongs to the dynamic harness; what a client asked for is
 * plain string work, and it is where a subscriber quietly gets the wrong feed
 * — a typo that parsed as "everything" would look exactly like an event that
 * never fires. */

#include "test.h"
#include "ipc_events.h"
#include <stdint.h>

static uint32_t parse_ok(const char *list) {
    uint32_t mask = 0;
    char err[320] = "";
    if (!fwm_ipc_events_parse(list, &mask, err, sizeof err)) {
        T_FAIL("parse(\"%s\") failed: %s", list ? list : "(null)", err);
        return 0;
    }
    return mask;
}

static void expect_reject(const char *list) {
    uint32_t mask = 0xdeadbeef;
    char err[320] = "";
    t_checks++;
    if (fwm_ipc_events_parse(list, &mask, err, sizeof err)) {
        T_FAIL("parse(\"%s\") should have failed", list ? list : "(null)");
        return;
    }
    /* The mask must survive a rejection untouched: a partial subscription
     * would be the one outcome the client cannot detect. */
    CHECK_INT(mask, 0xdeadbeef);
    CHECK(err[0] != '\0');
}

int main(void) {
    CASE("no argument means everything");
    CHECK_INT(parse_ok(NULL), FWM_EV_ALL);
    CHECK_INT(parse_ok(""), FWM_EV_ALL);
    CHECK_INT(parse_ok("all"), FWM_EV_ALL);
    /* Only separators is "they named nothing", not "they named an empty
     * event" — fwmctl passing through a stray space must not be an error. */
    CHECK_INT(parse_ok("   "), FWM_EV_ALL);
    CHECK_INT(parse_ok(" , "), FWM_EV_ALL);

    CASE("single event");
    CHECK_INT(parse_ok("window_open"), FWM_EV_WINDOW_OPEN);
    CHECK_INT(parse_ok("config_reload"), FWM_EV_CONFIG_RELOAD);

    CASE("lists, in either separator");
    CHECK_INT(parse_ok("window_open,window_close"),
              FWM_EV_WINDOW_OPEN | FWM_EV_WINDOW_CLOSE);
    CHECK_INT(parse_ok("window_open window_close"),
              FWM_EV_WINDOW_OPEN | FWM_EV_WINDOW_CLOSE);
    CHECK_INT(parse_ok("window_open, window_close ,desktop"),
              FWM_EV_WINDOW_OPEN | FWM_EV_WINDOW_CLOSE | FWM_EV_DESKTOP);
    /* Repeats collapse rather than counting. */
    CHECK_INT(parse_ok("gravity,gravity"), FWM_EV_GRAVITY);

    CASE("a bad name fails the whole request");
    expect_reject("nonsense");
    expect_reject("window_open,nonsense");
    expect_reject("nonsense,window_open");
    /* Prefixes and near-misses are not names. Matching is length-exact, so
     * "window" must not resolve to window_open by being a prefix of it. */
    expect_reject("window");
    expect_reject("window_opened");
    /* "all" is a whole-request word: accepting it inside a list would make
     * the rest of that list a lie about what arrives. */
    expect_reject("all,desktop");

    CASE("names round-trip with their bits");
    for (int i = 0; i < FWM_EV_COUNT; i++) {
        uint32_t bit = 1u << i;
        const char *name = fwm_ipc_event_name(bit);
        CHECK_NOT_NULL(name);
        if (!name) continue;
        CHECK_INT(parse_ok(name), bit);
    }
    /* Not a single bit, so not a single name. */
    CHECK_NULL(fwm_ipc_event_name(FWM_EV_ALL));
    CHECK_NULL(fwm_ipc_event_name(0));
    CHECK_NULL(fwm_ipc_event_name(1u << FWM_EV_COUNT));

    CASE("the help list names every event");
    char list[512];
    fwm_ipc_event_list(list, sizeof list);
    for (int i = 0; i < FWM_EV_COUNT; i++)
        CHECK(strstr(list, fwm_ipc_event_name(1u << i)) != NULL);

    return t_report("ipc_events");
}
