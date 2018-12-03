/* Copyright (c) 2017-2018, The Tor Project, Inc. */
/* Copyright (c) 2017, isis agora lovecruft  */
/* See LICENSE for licensing information     */

/**
 * \file test_router.c
 * \brief Unittests for code in router.c
 **/

#define ROUTER_PRIVATE

#include "core/or/or.h"
#include "app/config/config.h"
#include "core/mainloop/mainloop.h"
#include "feature/hibernate/hibernate.h"
#include "feature/nodelist/networkstatus.h"
#include "feature/nodelist/networkstatus_st.h"
#include "feature/nodelist/routerinfo_st.h"
#include "feature/nodelist/routerlist.h"
#include "feature/nodelist/routerstatus_st.h"
#include "feature/relay/router.h"
#include "feature/stats/rephist.h"
#include "lib/crypt_ops/crypto_curve25519.h"
#include "lib/crypt_ops/crypto_ed25519.h"

/* Test suite stuff */
#include "test/test.h"
#include "test/log_test_helpers.h"

NS_DECL(const routerinfo_t *, router_get_my_routerinfo, (void));

static routerinfo_t* mock_routerinfo;

static const routerinfo_t*
NS(router_get_my_routerinfo)(void)
{
  crypto_pk_t* ident_key;
  crypto_pk_t* tap_key;
  time_t now;

  if (!mock_routerinfo) {
    /* Mock the published timestamp, otherwise router_dump_router_to_string()
     * will poop its pants. */
    time(&now);

    /* We'll need keys, or router_dump_router_to_string() would return NULL. */
    ident_key = pk_generate(0);
    tap_key = pk_generate(0);

    tor_assert(ident_key != NULL);
    tor_assert(tap_key != NULL);

    mock_routerinfo = tor_malloc_zero(sizeof(routerinfo_t));
    mock_routerinfo->nickname = tor_strdup("ConlonNancarrow");
    mock_routerinfo->addr = 123456789;
    mock_routerinfo->or_port = 443;
    mock_routerinfo->platform = tor_strdup("unittest");
    mock_routerinfo->cache_info.published_on = now;
    mock_routerinfo->identity_pkey = crypto_pk_dup_key(ident_key);
    router_set_rsa_onion_pkey(tap_key, &mock_routerinfo->onion_pkey,
                              &mock_routerinfo->onion_pkey_len);
    mock_routerinfo->bandwidthrate = 9001;
    mock_routerinfo->bandwidthburst = 9002;
    crypto_pk_free(ident_key);
    crypto_pk_free(tap_key);
  }

  return mock_routerinfo;
}

/* If no distribution option was set, then check_bridge_distribution_setting()
 * should have set it to "any". */
static void
test_router_dump_router_to_string_no_bridge_distribution_method(void *arg)
{
  const char* needle = "bridge-distribution-request any";
  or_options_t* options = get_options_mutable();
  routerinfo_t* router = NULL;
  curve25519_keypair_t ntor_keypair;
  ed25519_keypair_t signing_keypair;
  char* desc = NULL;
  char* found = NULL;
  (void)arg;

  NS_MOCK(router_get_my_routerinfo);

  options->ORPort_set = 1;
  options->BridgeRelay = 1;

  /* Generate keys which router_dump_router_to_string() expects to exist. */
  tt_int_op(0, ==, curve25519_keypair_generate(&ntor_keypair, 0));
  tt_int_op(0, ==, ed25519_keypair_generate(&signing_keypair, 0));

  /* Set up part of our routerinfo_t so that we don't trigger any other
   * assertions in router_dump_router_to_string(). */
  router = (routerinfo_t*)router_get_my_routerinfo();
  tt_ptr_op(router, !=, NULL);

  router->onion_curve25519_pkey = &ntor_keypair.pubkey;

  /* Generate our server descriptor and ensure that the substring
   * "bridge-distribution-request any" occurs somewhere within it. */
  crypto_pk_t *onion_pkey = router_get_rsa_onion_pkey(router->onion_pkey,
                                                      router->onion_pkey_len);
  desc = router_dump_router_to_string(router,
                                      router->identity_pkey,
                                      onion_pkey,
                                      &ntor_keypair,
                                      &signing_keypair);
  crypto_pk_free(onion_pkey);
  tt_ptr_op(desc, !=, NULL);
  found = strstr(desc, needle);
  tt_ptr_op(found, !=, NULL);

 done:
  NS_UNMOCK(router_get_my_routerinfo);

  tor_free(desc);
}

static routerinfo_t *mock_router_get_my_routerinfo_result = NULL;

static const routerinfo_t *
mock_router_get_my_routerinfo(void)
{
  return mock_router_get_my_routerinfo_result;
}

static long
mock_get_uptime_3h(void)
{
  return 3*60*60;
}

static long
mock_get_uptime_1d(void)
{
  return 24*60*60;
}

static int
mock_rep_hist_bandwidth_assess(void)
{
  return 20001;
}

static int
mock_we_are_not_hibernating(void)
{
  return 0;
}

static int
mock_we_are_hibernating(void)
{
  return 0;
}

static void
test_router_check_descriptor_bandwidth_changed(void *arg)
{
  (void)arg;
  routerinfo_t routerinfo;
  memset(&routerinfo, 0, sizeof(routerinfo));
  mock_router_get_my_routerinfo_result = NULL;

  MOCK(we_are_hibernating, mock_we_are_not_hibernating);
  MOCK(router_get_my_routerinfo, mock_router_get_my_routerinfo);
  mock_router_get_my_routerinfo_result = &routerinfo;

  /* When uptime is less than 24h, no previous bandwidth, no last_changed
   * Uptime: 10800, last_changed: 0, Previous bw: 0, Current bw: 0 */
  routerinfo.bandwidthcapacity = 0;
  MOCK(get_uptime, mock_get_uptime_3h);
  setup_full_capture_of_logs(LOG_INFO);
  check_descriptor_bandwidth_changed(time(NULL));
  expect_log_msg_not_containing(
     "Measured bandwidth has changed; rebuilding descriptor.");
  teardown_capture_of_logs();

  /* When uptime is less than 24h, previous bandwidth,
   * last_changed more than 3h ago
   * Uptime: 10800, last_changed: 0, Previous bw: 10000, Current bw: 0 */
  routerinfo.bandwidthcapacity = 10000;
  setup_full_capture_of_logs(LOG_INFO);
  check_descriptor_bandwidth_changed(time(NULL));
  expect_log_msg_containing(
     "Measured bandwidth has changed; rebuilding descriptor.");
  teardown_capture_of_logs();

  /* When uptime is less than 24h, previous bandwidth,
   * last_changed more than 3h ago, and hibernating
   * Uptime: 10800, last_changed: 0, Previous bw: 10000, Current bw: 0 */

  UNMOCK(we_are_hibernating);
  MOCK(we_are_hibernating, mock_we_are_hibernating);
  routerinfo.bandwidthcapacity = 10000;
  setup_full_capture_of_logs(LOG_INFO);
  check_descriptor_bandwidth_changed(time(NULL));
  expect_log_msg_not_containing(
     "Measured bandwidth has changed; rebuilding descriptor.");
  teardown_capture_of_logs();
  UNMOCK(we_are_hibernating);
  MOCK(we_are_hibernating, mock_we_are_not_hibernating);

  /* When uptime is less than 24h, last_changed is not more than 3h ago
   * Uptime: 10800, last_changed: x, Previous bw: 10000, Current bw: 0 */
  setup_full_capture_of_logs(LOG_INFO);
  check_descriptor_bandwidth_changed(time(NULL));
  expect_log_msg_not_containing(
     "Measured bandwidth has changed; rebuilding descriptor.");
  teardown_capture_of_logs();

  /* When uptime is less than 24h and bandwidthcapacity does not change
   * Uptime: 10800, last_changed: x, Previous bw: 10000, Current bw: 20001 */
  MOCK(rep_hist_bandwidth_assess, mock_rep_hist_bandwidth_assess);
  setup_full_capture_of_logs(LOG_INFO);
  check_descriptor_bandwidth_changed(time(NULL) + 6*60*60 + 1);
  expect_log_msg_containing(
     "Measured bandwidth has changed; rebuilding descriptor.");
  UNMOCK(get_uptime);
  UNMOCK(rep_hist_bandwidth_assess);
  teardown_capture_of_logs();

  /* When uptime is more than 24h */
  MOCK(get_uptime, mock_get_uptime_1d);
  setup_full_capture_of_logs(LOG_INFO);
  check_descriptor_bandwidth_changed(time(NULL));
  expect_log_msg_not_containing(
     "Measured bandwidth has changed; rebuilding descriptor.");
  teardown_capture_of_logs();

 done:
  UNMOCK(get_uptime);
  UNMOCK(router_get_my_routerinfo);
  UNMOCK(we_are_hibernating);
}

static networkstatus_t *mock_ns = NULL;
static networkstatus_t *
mock_networkstatus_get_live_consensus(time_t now)
{
  (void)now;
  return mock_ns;
}

static routerstatus_t *mock_rs = NULL;
static const routerstatus_t *
mock_networkstatus_vote_find_entry(networkstatus_t *ns, const char *digest)
{
  (void)ns;
  (void)digest;
  return mock_rs;
}

static void
test_router_mark_if_too_old(void *arg)
{
  (void)arg;
  time_t now = approx_time();
  MOCK(networkstatus_get_live_consensus,
       mock_networkstatus_get_live_consensus);
  MOCK(networkstatus_vote_find_entry, mock_networkstatus_vote_find_entry);


  routerstatus_t rs;
  networkstatus_t ns;
  memset(&rs, 0, sizeof(rs));
  memset(&ns, 0, sizeof(ns));
  mock_ns = &ns;
  mock_ns->valid_after = now-3600;
  mock_rs = &rs;
  mock_rs->published_on = now - 10;

  // no reason to mark this time.
  desc_clean_since = now-10;
  desc_dirty_reason = NULL;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, now-10);

  // Doesn't appear in consensus?  Still don't mark it.
  mock_ns = NULL;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, now-10);
  mock_ns = &ns;

  // No new descriptor in a long time?  Mark it.
  desc_clean_since = now - 3600 * 96;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, 0);
  tt_str_op(desc_dirty_reason, OP_EQ, "time for new descriptor");

  // Version in consensus published a long time ago?  We won't mark it
  // if it's been clean for only a short time.
  desc_clean_since = now - 10;
  desc_dirty_reason = NULL;
  mock_rs->published_on = now - 3600 * 96;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, now - 10);

  // ... but if it's been clean a while, we mark.
  desc_clean_since = now - 2 * 3600;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, 0);
  tt_str_op(desc_dirty_reason, OP_EQ,
            "version listed in consensus is quite old");

  // same deal if we're marked stale.
  desc_clean_since = now - 2 * 3600;
  desc_dirty_reason = NULL;
  mock_rs->published_on = now - 10;
  mock_rs->is_staledesc = 1;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, 0);
  tt_str_op(desc_dirty_reason, OP_EQ,
            "listed as stale in consensus");

  // same deal if we're absent from the consensus.
  desc_clean_since = now - 2 * 3600;
  desc_dirty_reason = NULL;
  mock_rs = NULL;
  mark_my_descriptor_dirty_if_too_old(now);
  tt_i64_op(desc_clean_since, OP_EQ, 0);
  tt_str_op(desc_dirty_reason, OP_EQ,
            "not listed in consensus");

 done:
  UNMOCK(networkstatus_get_live_consensus);
  UNMOCK(networkstatus_vote_find_entry);
}

#define ROUTER_TEST(name, flags)                          \
  { #name, test_router_ ## name, flags, NULL, NULL }

struct testcase_t router_tests[] = {
  ROUTER_TEST(check_descriptor_bandwidth_changed, TT_FORK),
  ROUTER_TEST(dump_router_to_string_no_bridge_distribution_method, TT_FORK),
  ROUTER_TEST(mark_if_too_old, TT_FORK),
  END_OF_TESTCASES
};
