/* Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2020, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file rephist.c
 * \brief Basic history and performance-tracking functionality.
 *
 * Basic history and performance-tracking functionality to remember
 *    which servers have worked in the past, how much bandwidth we've
 *    been using, which ports we tend to want, and so on; further,
 *    exit port statistics, cell statistics, and connection statistics.
 *
 * The history and information tracked in this module could sensibly be
 * divided into several categories:
 *
 * <ul><li>Statistics used by authorities to remember the uptime and
 * stability information about various relays, including "uptime",
 * "weighted fractional uptime" and "mean time between failures".
 *
 * <li>Bandwidth usage history, used by relays to self-report how much
 * bandwidth they've used for different purposes over last day or so,
 * in order to generate the {dirreq-,}{read,write}-history lines in
 * that they publish.
 *
 * <li>Predicted ports, used by clients to remember how long it's been
 * since they opened an exit connection to each given target
 * port. Clients use this information in order to try to keep circuits
 * open to exit nodes that can connect to the ports that they care
 * about.  (The predicted ports mechanism also handles predicted circuit
 * usage that _isn't_ port-specific, such as resolves, internal circuits,
 * and so on.)
 *
 * <li>Public key operation counters, for tracking how many times we've
 * done each public key operation.  (This is unmaintained and we should
 * remove it.)
 *
 * <li>Exit statistics by port, used by exits to keep track of the
 * number of streams and bytes they've served at each exit port, so they
 * can generate their exit-kibibytes-{read,written} and
 * exit-streams-opened statistics.
 *
 * <li>Circuit stats, used by relays instances to tract circuit
 * queue fullness and delay over time, and generate cell-processed-cells,
 * cell-queued-cells, cell-time-in-queue, and cell-circuits-per-decile
 * statistics.
 *
 * <li>Descriptor serving statistics, used by directory caches to track
 * how many descriptors they've served.
 *
 * <li>Connection statistics, used by relays to track one-way and
 * bidirectional connections.
 *
 * <li>Onion handshake statistics, used by relays to count how many
 * TAP and ntor handshakes they've handled.
 *
 * <li>Hidden service statistics, used by relays to count rendezvous
 * traffic and HSDir-stored descriptors.
 *
 * <li>Link protocol statistics, used by relays to count how many times
 * each link protocol has been used.
 *
 * </ul>
 *
 * The entry points for this module are scattered throughout the
 * codebase.  Sending data, receiving data, connecting to a relay,
 * losing a connection to a relay, and so on can all trigger a change in
 * our current stats.  Relays also invoke this module in order to
 * extract their statistics when building routerinfo and extrainfo
 * objects in router.c.
 *
 * TODO: This module should be broken up.
 *
 * (The "rephist" name originally stood for "reputation and history". )
 **/

#define REPHIST_PRIVATE
#include "core/or/or.h"
#include "app/config/config.h"
#include "app/config/statefile.h"
#include "core/or/circuitlist.h"
#include "core/or/connection_or.h"
#include "feature/dirauth/authmode.h"
#include "feature/nodelist/networkstatus.h"
#include "feature/nodelist/nodelist.h"
#include "feature/relay/routermode.h"
#include "feature/stats/predict_ports.h"
#include "feature/stats/rephist.h"
#include "lib/container/order.h"
#include "lib/crypt_ops/crypto_rand.h"
#include "lib/math/laplace.h"

#include "feature/nodelist/networkstatus_st.h"
#include "core/or/or_circuit_st.h"
#include "app/config/or_state_st.h"

#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif

static void bw_arrays_init(void);

/** Total number of bytes currently allocated in fields used by rephist.c. */
uint64_t rephist_total_alloc = 0;
/** Number of or_history_t objects currently allocated. */
uint32_t rephist_total_num = 0;

/** If the total weighted run count of all runs for a router ever falls
 * below this amount, the router can be treated as having 0 MTBF. */
#define STABILITY_EPSILON 0.0001
/** Value by which to discount all old intervals for MTBF purposes.  This
 * is compounded every STABILITY_INTERVAL. */
#define STABILITY_ALPHA 0.95
/** Interval at which to discount all old intervals for MTBF purposes. */
#define STABILITY_INTERVAL (12 * 60 * 60)
/* (This combination of ALPHA, INTERVAL, and EPSILON makes it so that an
 * interval that just ended counts twice as much as one that ended a week ago,
 * 20X as much as one that ended a month ago, and routers that have had no
 * uptime data for about half a year will get forgotten.) */

/** History of an OR. */
typedef struct or_history_t {
  /** When did we start tracking this OR? */
  time_t since;
  /** When did we most recently note a change to this OR? */
  time_t changed;

  /** The address at which we most recently connected to this OR
   * successfully. */
  tor_addr_t last_reached_addr;

  /** The port at which we most recently connected to this OR successfully */
  uint16_t last_reached_port;

  /* === For MTBF tracking: */
  /** Weighted sum total of all times that this router has been online.
   */
  unsigned long weighted_run_length;
  /** If the router is now online (according to stability-checking rules),
   * when did it come online? */
  time_t start_of_run;
  /** Sum of weights for runs in weighted_run_length. */
  double total_run_weights;
  /* === For fractional uptime tracking: */
  time_t start_of_downtime;
  unsigned long weighted_uptime;
  unsigned long total_weighted_time;
} or_history_t;

/**
 * This structure holds accounting needed to calculate the padding overhead.
 */
typedef struct padding_counts_t {
  /** Total number of cells we have received, including padding */
  uint64_t read_cell_count;
  /** Total number of cells we have sent, including padding */
  uint64_t write_cell_count;
  /** Total number of CELL_PADDING cells we have received */
  uint64_t read_pad_cell_count;
  /** Total number of CELL_PADDING cells we have sent */
  uint64_t write_pad_cell_count;
  /** Total number of read cells on padding-enabled conns */
  uint64_t enabled_read_cell_count;
  /** Total number of sent cells on padding-enabled conns */
  uint64_t enabled_write_cell_count;
  /** Total number of read CELL_PADDING cells on padding-enabled cons */
  uint64_t enabled_read_pad_cell_count;
  /** Total number of sent CELL_PADDING cells on padding-enabled cons */
  uint64_t enabled_write_pad_cell_count;
  /** Total number of RELAY_DROP cells we have received */
  uint64_t read_drop_cell_count;
  /** Total number of RELAY_DROP cells we have sent */
  uint64_t write_drop_cell_count;
  /** The maximum number of padding timers we've seen in 24 hours */
  uint64_t maximum_chanpad_timers;
  /** When did we first copy padding_current into padding_published? */
  char first_published_at[ISO_TIME_LEN + 1];
} padding_counts_t;

/** Holds the current values of our padding statistics.
 * It is not published until it is transferred to padding_published. */
static padding_counts_t padding_current;

/** Remains fixed for a 24 hour period, and then is replaced
 * by a redacted copy of padding_current */
static padding_counts_t padding_published;

/** When did we last multiply all routers' weighted_run_length and
 * total_run_weights by STABILITY_ALPHA? */
static time_t stability_last_downrated = 0;

/**  */
static time_t started_tracking_stability = 0;

/** Map from hex OR identity digest to or_history_t. */
static digestmap_t *history_map = NULL;

/** Return the or_history_t for the OR with identity digest <b>id</b>,
 * creating it if necessary. */
static or_history_t *
get_or_history(const char *id)
{
  or_history_t *hist;

  if (tor_digest_is_zero(id))
    return NULL;

  hist = digestmap_get(history_map, id);
  if (!hist) {
    hist = tor_malloc_zero(sizeof(or_history_t));
    rephist_total_alloc += sizeof(or_history_t);
    rephist_total_num++;
    hist->since = hist->changed = time(NULL);
    tor_addr_make_unspec(&hist->last_reached_addr);
    digestmap_set(history_map, id, hist);
  }
  return hist;
}

/** Helper: free storage held by a single OR history entry. */
static void
free_or_history(void *_hist)
{
  or_history_t *hist = _hist;
  rephist_total_alloc -= sizeof(or_history_t);
  rephist_total_num--;
  tor_free(hist);
}

/** Initialize the static data structures for tracking history. */
void
rep_hist_init(void)
{
  history_map = digestmap_new();
  bw_arrays_init();
}

/** We have just decided that this router with identity digest <b>id</b> is
 * reachable, meaning we will give it a "Running" flag for the next while. */
void
rep_hist_note_router_reachable(const char *id, const tor_addr_t *at_addr,
                               const uint16_t at_port, time_t when)
{
  or_history_t *hist = get_or_history(id);
  int was_in_run = 1;
  char tbuf[ISO_TIME_LEN + 1];
  int addr_changed, port_changed;

  tor_assert(hist);
  tor_assert((!at_addr && !at_port) || (at_addr && at_port));

  addr_changed =
      at_addr && !tor_addr_is_null(&hist->last_reached_addr) &&
      tor_addr_compare(at_addr, &hist->last_reached_addr, CMP_EXACT) != 0;
  port_changed =
      at_port && hist->last_reached_port && at_port != hist->last_reached_port;

  if (!started_tracking_stability)
    started_tracking_stability = time(NULL);
  if (!hist->start_of_run) {
    hist->start_of_run = when;
    was_in_run = 0;
  }
  if (hist->start_of_downtime) {
    long down_length;

    format_local_iso_time(tbuf, hist->start_of_downtime);
    log_info(LD_HIST, "Router %s is now Running; it had been down since %s.",
             hex_str(id, DIGEST_LEN), tbuf);
    if (was_in_run)
      log_info(LD_HIST, "  (Paradoxically, it was already Running too.)");

    down_length = when - hist->start_of_downtime;
    hist->total_weighted_time += down_length;
    hist->start_of_downtime = 0;
  } else if (addr_changed || port_changed) {
    /* If we're reachable, but the address changed, treat this as some
     * downtime. */
    int penalty = get_options()->TestingTorNetwork ? 240 : 3600;
    networkstatus_t *ns;

    if ((ns = networkstatus_get_latest_consensus())) {
      int fresh_interval = (int)(ns->fresh_until - ns->valid_after);
      int live_interval = (int)(ns->valid_until - ns->valid_after);
      /* on average, a descriptor addr change takes .5 intervals to make it
       * into a consensus, and half a liveness period to make it to
       * clients. */
      penalty = (int)(fresh_interval + live_interval) / 2;
    }
    format_local_iso_time(tbuf, hist->start_of_run);
    log_info(LD_HIST,
             "Router %s still seems Running, but its address appears "
             "to have changed since the last time it was reachable.  I'm "
             "going to treat it as having been down for %d seconds",
             hex_str(id, DIGEST_LEN), penalty);
    rep_hist_note_router_unreachable(id, when - penalty);
    rep_hist_note_router_reachable(id, NULL, 0, when);
  } else {
    format_local_iso_time(tbuf, hist->start_of_run);
    if (was_in_run)
      log_debug(LD_HIST,
                "Router %s is still Running; it has been Running "
                "since %s",
                hex_str(id, DIGEST_LEN), tbuf);
    else
      log_info(LD_HIST,
               "Router %s is now Running; it was previously untracked",
               hex_str(id, DIGEST_LEN));
  }
  if (at_addr)
    tor_addr_copy(&hist->last_reached_addr, at_addr);
  if (at_port)
    hist->last_reached_port = at_port;
}

/** We have just decided that this router is unreachable, meaning
 * we are taking away its "Running" flag. */
void
rep_hist_note_router_unreachable(const char *id, time_t when)
{
  or_history_t *hist = get_or_history(id);
  char tbuf[ISO_TIME_LEN + 1];
  int was_running = 0;
  if (!started_tracking_stability)
    started_tracking_stability = time(NULL);

  tor_assert(hist);
  if (hist->start_of_run) {
    /*XXXX We could treat failed connections differently from failed
     * connect attempts. */
    long run_length = when - hist->start_of_run;
    format_local_iso_time(tbuf, hist->start_of_run);

    hist->total_run_weights += 1.0;
    hist->start_of_run = 0;
    if (run_length < 0) {
      unsigned long penalty = -run_length;
#define SUBTRACT_CLAMPED(var, penalty)                 \
  do {                                                 \
    (var) = (var) < (penalty) ? 0 : (var) - (penalty); \
  } while (0)

      SUBTRACT_CLAMPED(hist->weighted_run_length, penalty);
      SUBTRACT_CLAMPED(hist->weighted_uptime, penalty);
    } else {
      hist->weighted_run_length += run_length;
      hist->weighted_uptime += run_length;
      hist->total_weighted_time += run_length;
    }
    was_running = 1;
    log_info(LD_HIST,
             "Router %s is now non-Running: it had previously been "
             "Running since %s.  Its total weighted uptime is %lu/%lu.",
             hex_str(id, DIGEST_LEN), tbuf, hist->weighted_uptime,
             hist->total_weighted_time);
  }
  if (!hist->start_of_downtime) {
    hist->start_of_downtime = when;

    if (!was_running)
      log_info(LD_HIST,
               "Router %s is now non-Running; it was previously "
               "untracked.",
               hex_str(id, DIGEST_LEN));
  } else {
    if (!was_running) {
      format_local_iso_time(tbuf, hist->start_of_downtime);

      log_info(LD_HIST,
               "Router %s is still non-Running; it has been "
               "non-Running since %s.",
               hex_str(id, DIGEST_LEN), tbuf);
    }
  }
}

/** Mark a router with ID <b>id</b> as non-Running, and retroactively declare
 * that it has never been running: give it no stability and no WFU. */
void
rep_hist_make_router_pessimal(const char *id, time_t when)
{
  or_history_t *hist = get_or_history(id);
  tor_assert(hist);

  rep_hist_note_router_unreachable(id, when);

  hist->weighted_run_length = 0;
  hist->weighted_uptime = 0;
}

/** Helper: Discount all old MTBF data, if it is time to do so.  Return
 * the time at which we should next discount MTBF data. */
time_t
rep_hist_downrate_old_runs(time_t now)
{
  digestmap_iter_t *orhist_it;
  const char *digest1;
  or_history_t *hist;
  void *hist_p;
  double alpha = 1.0;

  if (!history_map)
    history_map = digestmap_new();
  if (!stability_last_downrated)
    stability_last_downrated = now;
  if (stability_last_downrated + STABILITY_INTERVAL > now)
    return stability_last_downrated + STABILITY_INTERVAL;

  /* Okay, we should downrate the data.  By how much? */
  while (stability_last_downrated + STABILITY_INTERVAL < now) {
    stability_last_downrated += STABILITY_INTERVAL;
    alpha *= STABILITY_ALPHA;
  }

  log_info(LD_HIST, "Discounting all old stability info by a factor of %f",
           alpha);

  /* Multiply every w_r_l, t_r_w pair by alpha. */
  for (orhist_it = digestmap_iter_init(history_map);
       !digestmap_iter_done(orhist_it);
       orhist_it = digestmap_iter_next(history_map, orhist_it)) {
    digestmap_iter_get(orhist_it, &digest1, &hist_p);
    hist = hist_p;

    hist->weighted_run_length =
        (unsigned long)(hist->weighted_run_length * alpha);
    hist->total_run_weights *= alpha;

    hist->weighted_uptime = (unsigned long)(hist->weighted_uptime * alpha);
    hist->total_weighted_time =
        (unsigned long)(hist->total_weighted_time * alpha);
  }

  return stability_last_downrated + STABILITY_INTERVAL;
}

/** Helper: Return the weighted MTBF of the router with history <b>hist</b>. */
static double
get_stability(or_history_t *hist, time_t when)
{
  long total = hist->weighted_run_length;
  double total_weights = hist->total_run_weights;

  if (hist->start_of_run) {
    /* We're currently in a run.  Let total and total_weights hold the values
     * they would hold if the current run were to end now. */
    total += (when - hist->start_of_run);
    total_weights += 1.0;
  }
  if (total_weights < STABILITY_EPSILON) {
    /* Round down to zero, and avoid divide-by-zero. */
    return 0.0;
  }

  return total / total_weights;
}

/** Return the total amount of time we've been observing, with each run of
 * time downrated by the appropriate factor. */
static long
get_total_weighted_time(or_history_t *hist, time_t when)
{
  long total = hist->total_weighted_time;
  if (hist->start_of_run) {
    total += (when - hist->start_of_run);
  } else if (hist->start_of_downtime) {
    total += (when - hist->start_of_downtime);
  }
  return total;
}

/** Helper: Return the weighted percent-of-time-online of the router with
 * history <b>hist</b>. */
static double
get_weighted_fractional_uptime(or_history_t *hist, time_t when)
{
  long total = hist->total_weighted_time;
  long up = hist->weighted_uptime;

  if (hist->start_of_run) {
    long run_length = (when - hist->start_of_run);
    up += run_length;
    total += run_length;
  } else if (hist->start_of_downtime) {
    total += (when - hist->start_of_downtime);
  }

  if (!total) {
    /* Avoid calling anybody's uptime infinity (which should be impossible if
     * the code is working), or NaN (which can happen for any router we haven't
     * observed up or down yet). */
    return 0.0;
  }

  return ((double)up) / total;
}

/** Return how long the router whose identity digest is <b>id</b> has
 *  been reachable. Return 0 if the router is unknown or currently deemed
 *  unreachable. */
long
rep_hist_get_uptime(const char *id, time_t when)
{
  or_history_t *hist = get_or_history(id);
  if (!hist)
    return 0;
  if (!hist->start_of_run || when < hist->start_of_run)
    return 0;
  return when - hist->start_of_run;
}

/** Return an estimated MTBF for the router whose identity digest is
 * <b>id</b>. Return 0 if the router is unknown. */
double
rep_hist_get_stability(const char *id, time_t when)
{
  or_history_t *hist = get_or_history(id);
  if (!hist)
    return 0.0;

  return get_stability(hist, when);
}

/** Return an estimated percent-of-time-online for the router whose identity
 * digest is <b>id</b>. Return 0 if the router is unknown. */
double
rep_hist_get_weighted_fractional_uptime(const char *id, time_t when)
{
  or_history_t *hist = get_or_history(id);
  if (!hist)
    return 0.0;

  return get_weighted_fractional_uptime(hist, when);
}

/** Return a number representing how long we've known about the router whose
 * digest is <b>id</b>. Return 0 if the router is unknown.
 *
 * Be careful: this measure increases monotonically as we know the router for
 * longer and longer, but it doesn't increase linearly.
 */
long
rep_hist_get_weighted_time_known(const char *id, time_t when)
{
  or_history_t *hist = get_or_history(id);
  if (!hist)
    return 0;

  return get_total_weighted_time(hist, when);
}

/** Return true if we've been measuring MTBFs for long enough to
 * pronounce on Stability. */
int
rep_hist_have_measured_enough_stability(void)
{
  /* XXXX++ This doesn't do so well when we change our opinion
   * as to whether we're tracking router stability. */
  return started_tracking_stability < time(NULL) - 4 * 60 * 60;
}

/** Log all the reliability data we have remembered, with the chosen
 * severity.
 */
void
rep_hist_dump_stats(time_t now, int severity)
{
  digestmap_iter_t *orhist_it;
  const char *name1, *digest1;
  char hexdigest1[HEX_DIGEST_LEN + 1];
  or_history_t *or_history;
  void *or_history_p;
  const node_t *node;

  rep_history_clean(now - get_options()->RephistTrackTime);

  tor_log(severity, LD_HIST, "--------------- Dumping history information:");

  for (orhist_it = digestmap_iter_init(history_map);
       !digestmap_iter_done(orhist_it);
       orhist_it = digestmap_iter_next(history_map, orhist_it)) {
    double s;
    long stability;
    digestmap_iter_get(orhist_it, &digest1, &or_history_p);
    or_history = (or_history_t *)or_history_p;

    if ((node = node_get_by_id(digest1)) && node_get_nickname(node))
      name1 = node_get_nickname(node);
    else
      name1 = "(unknown)";
    base16_encode(hexdigest1, sizeof(hexdigest1), digest1, DIGEST_LEN);
    s = get_stability(or_history, now);
    stability = (long)s;
    tor_log(severity, LD_HIST, "OR %s [%s]: wmtbf %lu:%02lu:%02lu", name1,
            hexdigest1, stability / 3600, (stability / 60) % 60,
            stability % 60);
  }
}

/** Remove history info for routers/links that haven't changed since
 * <b>before</b>.
 */
void
rep_history_clean(time_t before)
{
  int authority = authdir_mode(get_options());
  or_history_t *or_history;
  void *or_history_p;
  digestmap_iter_t *orhist_it;
  const char *d1;

  orhist_it = digestmap_iter_init(history_map);
  while (!digestmap_iter_done(orhist_it)) {
    int should_remove;
    digestmap_iter_get(orhist_it, &d1, &or_history_p);
    or_history = or_history_p;

    should_remove = authority
                        ? (or_history->total_run_weights < STABILITY_EPSILON &&
                           !or_history->start_of_run)
                        : (or_history->changed < before);
    if (should_remove) {
      orhist_it = digestmap_iter_next_rmv(history_map, orhist_it);
      free_or_history(or_history);
      continue;
    }
    orhist_it = digestmap_iter_next(history_map, orhist_it);
  }
}

/** Write MTBF data to disk. Return 0 on success, negative on failure.
 *
 * If <b>missing_means_down</b>, then if we're about to write an entry
 * that is still considered up but isn't in our routerlist, consider it
 * to be down. */
int
rep_hist_record_mtbf_data(time_t now, int missing_means_down)
{
  char time_buf[ISO_TIME_LEN + 1];

  digestmap_iter_t *orhist_it;
  const char *digest;
  void *or_history_p;
  or_history_t *hist;
  open_file_t *open_file = NULL;
  FILE *f;

  {
    char *filename = get_datadir_fname("router-stability");
    f = start_writing_to_stdio_file(filename, OPEN_FLAGS_REPLACE | O_TEXT,
                                    0600, &open_file);
    tor_free(filename);
    if (!f)
      return -1;
  }

  /* File format is:
   *   FormatLine *KeywordLine Data
   *
   *   FormatLine = "format 1" NL
   *   KeywordLine = Keyword SP Arguments NL
   *   Data = "data" NL *RouterMTBFLine "." NL
   *   RouterMTBFLine = Fingerprint SP WeightedRunLen SP
   *           TotalRunWeights [SP S=StartRunTime] NL
   */
#define PUT(s)             \
  STMT_BEGIN               \
    if (fputs((s), f) < 0) \
      goto err;            \
  STMT_END
#define PRINTF(args)      \
  STMT_BEGIN              \
    if (fprintf args < 0) \
      goto err;           \
  STMT_END

  PUT("format 2\n");

  format_iso_time(time_buf, time(NULL));
  PRINTF((f, "stored-at %s\n", time_buf));

  if (started_tracking_stability) {
    format_iso_time(time_buf, started_tracking_stability);
    PRINTF((f, "tracked-since %s\n", time_buf));
  }
  if (stability_last_downrated) {
    format_iso_time(time_buf, stability_last_downrated);
    PRINTF((f, "last-downrated %s\n", time_buf));
  }

  PUT("data\n");

  /* XXX Nick: now bridge auths record this for all routers too.
   * Should we make them record it only for bridge routers? -RD
   * Not for 0.2.0. -NM */
  for (orhist_it = digestmap_iter_init(history_map);
       !digestmap_iter_done(orhist_it);
       orhist_it = digestmap_iter_next(history_map, orhist_it)) {
    char dbuf[HEX_DIGEST_LEN + 1];
    const char *t = NULL;
    digestmap_iter_get(orhist_it, &digest, &or_history_p);
    hist = (or_history_t *)or_history_p;

    base16_encode(dbuf, sizeof(dbuf), digest, DIGEST_LEN);

    if (missing_means_down && hist->start_of_run &&
        !connection_or_digest_is_known_relay(digest)) {
      /* We think this relay is running, but it's not listed in our
       * consensus. Somehow it fell out without telling us it went
       * down. Complain and also correct it. */
      log_info(LD_HIST,
               "Relay '%s' is listed as up in rephist, but it's not in "
               "our routerlist. Correcting.",
               dbuf);
      rep_hist_note_router_unreachable(digest, now);
    }

    PRINTF((f, "R %s\n", dbuf));
    if (hist->start_of_run > 0) {
      format_iso_time(time_buf, hist->start_of_run);
      t = time_buf;
    }
    PRINTF((f, "+MTBF %lu %.5f%s%s\n", hist->weighted_run_length,
            hist->total_run_weights, t ? " S=" : "", t ? t : ""));
    t = NULL;
    if (hist->start_of_downtime > 0) {
      format_iso_time(time_buf, hist->start_of_downtime);
      t = time_buf;
    }
    PRINTF((f, "+WFU %lu %lu%s%s\n", hist->weighted_uptime,
            hist->total_weighted_time, t ? " S=" : "", t ? t : ""));
  }

  PUT(".\n");

#undef PUT
#undef PRINTF

  return finish_writing_to_file(open_file);
err:
  abort_writing_to_file(open_file);
  return -1;
}

/** Helper: return the first j >= i such that !strcmpstart(sl[j], prefix) and
 * such that no line sl[k] with i <= k < j starts with "R ".  Return -1 if no
 * such line exists. */
static int
find_next_with(smartlist_t *sl, int i, const char *prefix)
{
  for (; i < smartlist_len(sl); ++i) {
    const char *line = smartlist_get(sl, i);
    if (!strcmpstart(line, prefix))
      return i;
    if (!strcmpstart(line, "R "))
      return -1;
  }
  return -1;
}

/** How many bad times has parse_possibly_bad_iso_time() parsed? */
static int n_bogus_times = 0;
/** Parse the ISO-formatted time in <b>s</b> into *<b>time_out</b>, but
 * round any pre-1970 date to Jan 1, 1970. */
static int
parse_possibly_bad_iso_time(const char *s, time_t *time_out)
{
  int year;
  char b[5];
  strlcpy(b, s, sizeof(b));
  b[4] = '\0';
  year = (int)tor_parse_long(b, 10, 0, INT_MAX, NULL, NULL);
  if (year < 1970) {
    *time_out = 0;
    ++n_bogus_times;
    return 0;
  } else
    return parse_iso_time(s, time_out);
}

/** We've read a time <b>t</b> from a file stored at <b>stored_at</b>, which
 * says we started measuring at <b>started_measuring</b>.  Return a new number
 * that's about as much before <b>now</b> as <b>t</b> was before
 * <b>stored_at</b>.
 */
static inline time_t
correct_time(time_t t, time_t now, time_t stored_at, time_t started_measuring)
{
  if (t < started_measuring - 24 * 60 * 60 * 365)
    return 0;
  else if (t < started_measuring)
    return started_measuring;
  else if (t > stored_at)
    return 0;
  else {
    long run_length = stored_at - t;
    t = (time_t)(now - run_length);
    if (t < started_measuring)
      t = started_measuring;
    return t;
  }
}

/** Load MTBF data from disk.  Returns 0 on success or recoverable error, -1
 * on failure. */
int
rep_hist_load_mtbf_data(time_t now)
{
  /* XXXX won't handle being called while history is already populated. */
  smartlist_t *lines;
  const char *line = NULL;
  int r = 0, i;
  time_t last_downrated = 0, stored_at = 0, tracked_since = 0;
  time_t latest_possible_start = now;
  long format = -1;

  {
    char *filename = get_datadir_fname("router-stability");
    char *d = read_file_to_str(filename, RFTS_IGNORE_MISSING, NULL);
    tor_free(filename);
    if (!d)
      return -1;
    lines = smartlist_new();
    smartlist_split_string(lines, d, "\n", SPLIT_SKIP_SPACE, 0);
    tor_free(d);
  }

  {
    const char *firstline;
    if (smartlist_len(lines) > 4) {
      firstline = smartlist_get(lines, 0);
      if (!strcmpstart(firstline, "format "))
        format = tor_parse_long(firstline + strlen("format "), 10, -1,
                                LONG_MAX, NULL, NULL);
    }
  }
  if (format != 1 && format != 2) {
    log_warn(LD_HIST, "Unrecognized format in mtbf history file. Skipping.");
    goto err;
  }
  for (i = 1; i < smartlist_len(lines); ++i) {
    line = smartlist_get(lines, i);
    if (!strcmp(line, "data"))
      break;
    if (!strcmpstart(line, "last-downrated ")) {
      if (parse_iso_time(line + strlen("last-downrated "), &last_downrated) <
          0)
        log_warn(LD_HIST, "Couldn't parse downrate time in mtbf "
                          "history file.");
    }
    if (!strcmpstart(line, "stored-at ")) {
      if (parse_iso_time(line + strlen("stored-at "), &stored_at) < 0)
        log_warn(LD_HIST, "Couldn't parse stored time in mtbf "
                          "history file.");
    }
    if (!strcmpstart(line, "tracked-since ")) {
      if (parse_iso_time(line + strlen("tracked-since "), &tracked_since) < 0)
        log_warn(LD_HIST, "Couldn't parse started-tracking time in mtbf "
                          "history file.");
    }
  }
  if (last_downrated > now)
    last_downrated = now;
  if (tracked_since > now)
    tracked_since = now;

  if (!stored_at) {
    log_warn(LD_HIST, "No stored time recorded.");
    goto err;
  }

  if (line && !strcmp(line, "data"))
    ++i;

  n_bogus_times = 0;

  for (; i < smartlist_len(lines); ++i) {
    char digest[DIGEST_LEN];
    char hexbuf[HEX_DIGEST_LEN + 1];
    char mtbf_timebuf[ISO_TIME_LEN + 1];
    char wfu_timebuf[ISO_TIME_LEN + 1];
    time_t start_of_run = 0;
    time_t start_of_downtime = 0;
    int have_mtbf = 0, have_wfu = 0;
    long wrl = 0;
    double trw = 0;
    long wt_uptime = 0, total_wt_time = 0;
    int n;
    or_history_t *hist;
    line = smartlist_get(lines, i);
    if (!strcmp(line, "."))
      break;

    mtbf_timebuf[0] = '\0';
    wfu_timebuf[0] = '\0';

    if (format == 1) {
      n = tor_sscanf(line, "%40s %ld %lf S=%10s %8s", hexbuf, &wrl, &trw,
                     mtbf_timebuf, mtbf_timebuf + 11);
      if (n != 3 && n != 5) {
        log_warn(LD_HIST, "Couldn't scan line %s", escaped(line));
        continue;
      }
      have_mtbf = 1;
    } else {
      // format == 2.
      int mtbf_idx, wfu_idx;
      if (strcmpstart(line, "R ") || strlen(line) < 2 + HEX_DIGEST_LEN)
        continue;
      strlcpy(hexbuf, line + 2, sizeof(hexbuf));
      mtbf_idx = find_next_with(lines, i + 1, "+MTBF ");
      wfu_idx = find_next_with(lines, i + 1, "+WFU ");
      if (mtbf_idx >= 0) {
        const char *mtbfline = smartlist_get(lines, mtbf_idx);
        n = tor_sscanf(mtbfline, "+MTBF %lu %lf S=%10s %8s", &wrl, &trw,
                       mtbf_timebuf, mtbf_timebuf + 11);
        if (n == 2 || n == 4) {
          have_mtbf = 1;
        } else {
          log_warn(LD_HIST, "Couldn't scan +MTBF line %s", escaped(mtbfline));
        }
      }
      if (wfu_idx >= 0) {
        const char *wfuline = smartlist_get(lines, wfu_idx);
        n = tor_sscanf(wfuline, "+WFU %lu %lu S=%10s %8s", &wt_uptime,
                       &total_wt_time, wfu_timebuf, wfu_timebuf + 11);
        if (n == 2 || n == 4) {
          have_wfu = 1;
        } else {
          log_warn(LD_HIST, "Couldn't scan +WFU line %s", escaped(wfuline));
        }
      }
      if (wfu_idx > i)
        i = wfu_idx;
      if (mtbf_idx > i)
        i = mtbf_idx;
    }
    if (base16_decode(digest, DIGEST_LEN, hexbuf, HEX_DIGEST_LEN) !=
        DIGEST_LEN) {
      log_warn(LD_HIST, "Couldn't hex string %s", escaped(hexbuf));
      continue;
    }
    hist = get_or_history(digest);
    if (!hist)
      continue;

    if (have_mtbf) {
      if (mtbf_timebuf[0]) {
        mtbf_timebuf[10] = ' ';
        if (parse_possibly_bad_iso_time(mtbf_timebuf, &start_of_run) < 0)
          log_warn(LD_HIST, "Couldn't parse time %s", escaped(mtbf_timebuf));
      }
      hist->start_of_run =
          correct_time(start_of_run, now, stored_at, tracked_since);
      if (hist->start_of_run < latest_possible_start + wrl)
        latest_possible_start = (time_t)(hist->start_of_run - wrl);

      hist->weighted_run_length = wrl;
      hist->total_run_weights = trw;
    }
    if (have_wfu) {
      if (wfu_timebuf[0]) {
        wfu_timebuf[10] = ' ';
        if (parse_possibly_bad_iso_time(wfu_timebuf, &start_of_downtime) < 0)
          log_warn(LD_HIST, "Couldn't parse time %s", escaped(wfu_timebuf));
      }
    }
    hist->start_of_downtime =
        correct_time(start_of_downtime, now, stored_at, tracked_since);
    hist->weighted_uptime = wt_uptime;
    hist->total_weighted_time = total_wt_time;
  }
  if (strcmp(line, "."))
    log_warn(LD_HIST, "Truncated MTBF file.");

  if (tracked_since < 86400 * 365) /* Recover from insanely early value. */
    tracked_since = latest_possible_start;

  stability_last_downrated = last_downrated;
  started_tracking_stability = tracked_since;

  goto done;
err:
  r = -1;
done:
  SMARTLIST_FOREACH(lines, char *, cp, tor_free(cp));
  smartlist_free(lines);
  return r;
}

/** For how many seconds do we keep track of individual per-second bandwidth
 * totals? */
#define NUM_SECS_ROLLING_MEASURE 10
/** How large are the intervals for which we track and report bandwidth use? */
#define NUM_SECS_BW_SUM_INTERVAL (24 * 60 * 60)
/** How far in the past do we remember and publish bandwidth use? */
#define NUM_SECS_BW_SUM_IS_VALID (5 * 24 * 60 * 60)
/** How many bandwidth usage intervals do we remember? (derived) */
#define NUM_TOTALS (NUM_SECS_BW_SUM_IS_VALID / NUM_SECS_BW_SUM_INTERVAL)

/** Structure to track bandwidth use, and remember the maxima for a given
 * time period.
 */
struct bw_array_t {
  /** Observation array: Total number of bytes transferred in each of the last
   * NUM_SECS_ROLLING_MEASURE seconds. This is used as a circular array. */
  uint64_t obs[NUM_SECS_ROLLING_MEASURE];
  int cur_obs_idx; /**< Current position in obs. */
  time_t cur_obs_time; /**< Time represented in obs[cur_obs_idx] */
  uint64_t total_obs; /**< Total for all members of obs except
                       * obs[cur_obs_idx] */
  uint64_t max_total; /**< Largest value that total_obs has taken on in the
                       * current period. */
  uint64_t total_in_period; /**< Total bytes transferred in the current
                             * period. */

  /** When does the next period begin? */
  time_t next_period;
  /** Where in 'maxima' should the maximum bandwidth usage for the current
   * period be stored? */
  int next_max_idx;
  /** How many values in maxima/totals have been set ever? */
  int num_maxes_set;
  /** Circular array of the maximum
   * bandwidth-per-NUM_SECS_ROLLING_MEASURE usage for the last
   * NUM_TOTALS periods */
  uint64_t maxima[NUM_TOTALS];
  /** Circular array of the total bandwidth usage for the last NUM_TOTALS
   * periods */
  uint64_t totals[NUM_TOTALS];
};

/** Shift the current period of b forward by one. */
STATIC void
commit_max(bw_array_t *b)
{
  /* Store total from current period. */
  b->totals[b->next_max_idx] = b->total_in_period;
  /* Store maximum from current period. */
  b->maxima[b->next_max_idx++] = b->max_total;
  /* Advance next_period and next_max_idx */
  b->next_period += NUM_SECS_BW_SUM_INTERVAL;
  if (b->next_max_idx == NUM_TOTALS)
    b->next_max_idx = 0;
  if (b->num_maxes_set < NUM_TOTALS)
    ++b->num_maxes_set;
  /* Reset max_total. */
  b->max_total = 0;
  /* Reset total_in_period. */
  b->total_in_period = 0;
}

/** Shift the current observation time of <b>b</b> forward by one second. */
STATIC void
advance_obs(bw_array_t *b)
{
  int nextidx;
  uint64_t total;

  /* Calculate the total bandwidth for the last NUM_SECS_ROLLING_MEASURE
   * seconds; adjust max_total as needed.*/
  total = b->total_obs + b->obs[b->cur_obs_idx];
  if (total > b->max_total)
    b->max_total = total;

  nextidx = b->cur_obs_idx + 1;
  if (nextidx == NUM_SECS_ROLLING_MEASURE)
    nextidx = 0;

  b->total_obs = total - b->obs[nextidx];
  b->obs[nextidx] = 0;
  b->cur_obs_idx = nextidx;

  if (++b->cur_obs_time >= b->next_period)
    commit_max(b);
}

/** Add <b>n</b> bytes to the number of bytes in <b>b</b> for second
 * <b>when</b>. */
static inline void
add_obs(bw_array_t *b, time_t when, uint64_t n)
{
  if (when < b->cur_obs_time)
    return; /* Don't record data in the past. */

  /* If we're currently adding observations for an earlier second than
   * 'when', advance b->cur_obs_time and b->cur_obs_idx by an
   * appropriate number of seconds, and do all the other housekeeping. */
  while (when > b->cur_obs_time) {
    /* Doing this one second at a time is potentially inefficient, if we start
       with a state file that is very old.  Fortunately, it doesn't seem to
       show up in profiles, so we can just ignore it for now. */
    advance_obs(b);
  }

  b->obs[b->cur_obs_idx] += n;
  b->total_in_period += n;
}

/** Allocate, initialize, and return a new bw_array. */
static bw_array_t *
bw_array_new(void)
{
  bw_array_t *b;
  time_t start;
  b = tor_malloc_zero(sizeof(bw_array_t));
  rephist_total_alloc += sizeof(bw_array_t);
  start = time(NULL);
  b->cur_obs_time = start;
  b->next_period = start + NUM_SECS_BW_SUM_INTERVAL;
  return b;
}

#define bw_array_free(val) FREE_AND_NULL(bw_array_t, bw_array_free_, (val))

/** Free storage held by bandwidth array <b>b</b>. */
static void
bw_array_free_(bw_array_t *b)
{
  if (!b) {
    return;
  }

  rephist_total_alloc -= sizeof(bw_array_t);
  tor_free(b);
}

/** Recent history of bandwidth observations for read operations. */
static bw_array_t *read_array = NULL;
/** Recent history of bandwidth observations for write operations. */
STATIC bw_array_t *write_array = NULL;
/** Recent history of bandwidth observations for read operations for the
    directory protocol. */
static bw_array_t *dir_read_array = NULL;
/** Recent history of bandwidth observations for write operations for the
    directory protocol. */
static bw_array_t *dir_write_array = NULL;

/** Set up [dir-]read_array and [dir-]write_array, freeing them if they
 * already exist. */
static void
bw_arrays_init(void)
{
  bw_array_free(read_array);
  bw_array_free(write_array);
  bw_array_free(dir_read_array);
  bw_array_free(dir_write_array);

  read_array = bw_array_new();
  write_array = bw_array_new();
  dir_read_array = bw_array_new();
  dir_write_array = bw_array_new();
}

/** Remember that we read <b>num_bytes</b> bytes in second <b>when</b>.
 *
 * Add num_bytes to the current running total for <b>when</b>.
 *
 * <b>when</b> can go back to time, but it's safe to ignore calls
 * earlier than the latest <b>when</b> you've heard of.
 */
void
rep_hist_note_bytes_written(uint64_t num_bytes, time_t when)
{
  /* Maybe a circular array for recent seconds, and step to a new point
   * every time a new second shows up. Or simpler is to just to have
   * a normal array and push down each item every second; it's short.
   */
  /* When a new second has rolled over, compute the sum of the bytes we've
   * seen over when-1 to when-1-NUM_SECS_ROLLING_MEASURE, and stick it
   * somewhere. See rep_hist_bandwidth_assess() below.
   */
  add_obs(write_array, when, num_bytes);
}

/** Remember that we wrote <b>num_bytes</b> bytes in second <b>when</b>.
 * (like rep_hist_note_bytes_written() above)
 */
void
rep_hist_note_bytes_read(uint64_t num_bytes, time_t when)
{
  /* if we're smart, we can make this func and the one above share code */
  add_obs(read_array, when, num_bytes);
}

/** Remember that we wrote <b>num_bytes</b> directory bytes in second
 * <b>when</b>. (like rep_hist_note_bytes_written() above)
 */
void
rep_hist_note_dir_bytes_written(uint64_t num_bytes, time_t when)
{
  add_obs(dir_write_array, when, num_bytes);
}

/** Remember that we read <b>num_bytes</b> directory bytes in second
 * <b>when</b>. (like rep_hist_note_bytes_written() above)
 */
void
rep_hist_note_dir_bytes_read(uint64_t num_bytes, time_t when)
{
  add_obs(dir_read_array, when, num_bytes);
}

/** Helper: Return the largest value in b->maxima.  (This is equal to the
 * most bandwidth used in any NUM_SECS_ROLLING_MEASURE period for the last
 * NUM_SECS_BW_SUM_IS_VALID seconds.)
 */
STATIC uint64_t
find_largest_max(bw_array_t *b)
{
  int i;
  uint64_t max;
  max = 0;
  for (i = 0; i < NUM_TOTALS; ++i) {
    if (b->maxima[i] > max)
      max = b->maxima[i];
  }
  return max;
}

/** Find the largest sums in the past NUM_SECS_BW_SUM_IS_VALID (roughly)
 * seconds. Find one sum for reading and one for writing. They don't have
 * to be at the same time.
 *
 * Return the smaller of these sums, divided by NUM_SECS_ROLLING_MEASURE.
 */
MOCK_IMPL(int,
rep_hist_bandwidth_assess, (void))
{
  uint64_t w, r;
  r = find_largest_max(read_array);
  w = find_largest_max(write_array);
  if (r > w)
    return (int)(((double)w) / NUM_SECS_ROLLING_MEASURE);
  else
    return (int)(((double)r) / NUM_SECS_ROLLING_MEASURE);
}

/** Print the bandwidth history of b (either [dir-]read_array or
 * [dir-]write_array) into the buffer pointed to by buf.  The format is
 * simply comma separated numbers, from oldest to newest.
 *
 * It returns the number of bytes written.
 */
static size_t
rep_hist_fill_bandwidth_history(char *buf, size_t len, const bw_array_t *b)
{
  char *cp = buf;
  int i, n;
  const or_options_t *options = get_options();
  uint64_t cutoff;

  if (b->num_maxes_set <= b->next_max_idx) {
    /* We haven't been through the circular array yet; time starts at i=0.*/
    i = 0;
  } else {
    /* We've been around the array at least once.  The next i to be
       overwritten is the oldest. */
    i = b->next_max_idx;
  }

  if (options->RelayBandwidthRate) {
    /* We don't want to report that we used more bandwidth than the max we're
     * willing to relay; otherwise everybody will know how much traffic
     * we used ourself. */
    cutoff = options->RelayBandwidthRate * NUM_SECS_BW_SUM_INTERVAL;
  } else {
    cutoff = UINT64_MAX;
  }

  for (n = 0; n < b->num_maxes_set; ++n, ++i) {
    uint64_t total;
    if (i >= NUM_TOTALS)
      i -= NUM_TOTALS;
    tor_assert(i < NUM_TOTALS);
    /* Round the bandwidth used down to the nearest 1k. */
    total = b->totals[i] & ~0x3ff;
    if (total > cutoff)
      total = cutoff;

    if (n == (b->num_maxes_set - 1))
      tor_snprintf(cp, len - (cp - buf), "%" PRIu64, (total));
    else
      tor_snprintf(cp, len - (cp - buf), "%" PRIu64 ",", (total));
    cp += strlen(cp);
  }
  return cp - buf;
}

/** Allocate and return lines for representing this server's bandwidth
 * history in its descriptor. We publish these lines in our extra-info
 * descriptor.
 */
char *
rep_hist_get_bandwidth_lines(void)
{
  char *buf, *cp;
  char t[ISO_TIME_LEN + 1];
  int r;
  bw_array_t *b = NULL;
  const char *desc = NULL;
  size_t len;

  /* [dirreq-](read|write)-history yyyy-mm-dd HH:MM:SS (n s) n,n,n... */
/* The n,n,n part above. Largest representation of a uint64_t is 20 chars
 * long, plus the comma. */
#define MAX_HIST_VALUE_LEN (21 * NUM_TOTALS)
  len = (67 + MAX_HIST_VALUE_LEN) * 4;
  buf = tor_malloc_zero(len);
  cp = buf;
  for (r = 0; r < 4; ++r) {
    char tmp[MAX_HIST_VALUE_LEN];
    size_t slen;
    switch (r) {
    case 0:
      b = write_array;
      desc = "write-history";
      break;
    case 1:
      b = read_array;
      desc = "read-history";
      break;
    case 2:
      b = dir_write_array;
      desc = "dirreq-write-history";
      break;
    case 3:
      b = dir_read_array;
      desc = "dirreq-read-history";
      break;
    }
    tor_assert(b);
    slen = rep_hist_fill_bandwidth_history(tmp, MAX_HIST_VALUE_LEN, b);
    /* If we don't have anything to write, skip to the next entry. */
    if (slen == 0)
      continue;
    format_iso_time(t, b->next_period - NUM_SECS_BW_SUM_INTERVAL);
    tor_snprintf(cp, len - (cp - buf), "%s %s (%d s) ", desc, t,
                 NUM_SECS_BW_SUM_INTERVAL);
    cp += strlen(cp);
    strlcat(cp, tmp, len - (cp - buf));
    cp += slen;
    strlcat(cp, "\n", len - (cp - buf));
    ++cp;
  }
  return buf;
}

/** Write a single bw_array_t into the Values, Ends, Interval, and Maximum
 * entries of an or_state_t. Done before writing out a new state file. */
static void
rep_hist_update_bwhist_state_section(or_state_t *state, const bw_array_t *b,
                                     smartlist_t **s_values,
                                     smartlist_t **s_maxima, time_t *s_begins,
                                     int *s_interval)
{
  int i, j;
  uint64_t maxval;

  if (*s_values) {
    SMARTLIST_FOREACH(*s_values, char *, val, tor_free(val));
    smartlist_free(*s_values);
  }
  if (*s_maxima) {
    SMARTLIST_FOREACH(*s_maxima, char *, val, tor_free(val));
    smartlist_free(*s_maxima);
  }
  if (!server_mode(get_options())) {
    /* Clients don't need to store bandwidth history persistently;
     * force these values to the defaults. */
    /* FFFF we should pull the default out of config.c's state table,
     * so we don't have two defaults. */
    if (*s_begins != 0 || *s_interval != 900) {
      time_t now = time(NULL);
      time_t save_at = get_options()->AvoidDiskWrites ? now + 3600 : now + 600;
      or_state_mark_dirty(state, save_at);
    }
    *s_begins = 0;
    *s_interval = 900;
    *s_values = smartlist_new();
    *s_maxima = smartlist_new();
    return;
  }
  *s_begins = b->next_period;
  *s_interval = NUM_SECS_BW_SUM_INTERVAL;

  *s_values = smartlist_new();
  *s_maxima = smartlist_new();
  /* Set i to first position in circular array */
  i = (b->num_maxes_set <= b->next_max_idx) ? 0 : b->next_max_idx;
  for (j = 0; j < b->num_maxes_set; ++j, ++i) {
    if (i >= NUM_TOTALS)
      i = 0;
    smartlist_add_asprintf(*s_values, "%" PRIu64, (b->totals[i] & ~0x3ff));
    maxval = b->maxima[i] / NUM_SECS_ROLLING_MEASURE;
    smartlist_add_asprintf(*s_maxima, "%" PRIu64, (maxval & ~0x3ff));
  }
  smartlist_add_asprintf(*s_values, "%" PRIu64, (b->total_in_period & ~0x3ff));
  maxval = b->max_total / NUM_SECS_ROLLING_MEASURE;
  smartlist_add_asprintf(*s_maxima, "%" PRIu64, (maxval & ~0x3ff));
}

/** Update <b>state</b> with the newest bandwidth history. Done before
 * writing out a new state file. */
void
rep_hist_update_state(or_state_t *state)
{
#define UPDATE(arrname, st)                                       \
  rep_hist_update_bwhist_state_section(                           \
      state, (arrname), &state->BWHistory##st##Values,            \
      &state->BWHistory##st##Maxima, &state->BWHistory##st##Ends, \
      &state->BWHistory##st##Interval)

  UPDATE(write_array, Write);
  UPDATE(read_array, Read);
  UPDATE(dir_write_array, DirWrite);
  UPDATE(dir_read_array, DirRead);

  if (server_mode(get_options())) {
    or_state_mark_dirty(state, time(NULL) + (2 * 3600));
  }
#undef UPDATE
}

/** Load a single bw_array_t from its Values, Ends, Maxima, and Interval
 * entries in an or_state_t. Done while reading the state file. */
static int
rep_hist_load_bwhist_state_section(bw_array_t *b, const smartlist_t *s_values,
                                   const smartlist_t *s_maxima,
                                   const time_t s_begins, const int s_interval)
{
  time_t now = time(NULL);
  int retval = 0;
  time_t start;

  uint64_t v, mv;
  int i, ok, ok_m = 0;
  int have_maxima = s_maxima && s_values &&
                    (smartlist_len(s_values) == smartlist_len(s_maxima));

  if (s_values && s_begins >= now - NUM_SECS_BW_SUM_INTERVAL * NUM_TOTALS) {
    start = s_begins - s_interval * (smartlist_len(s_values));
    if (start > now)
      return 0;
    b->cur_obs_time = start;
    b->next_period = start + NUM_SECS_BW_SUM_INTERVAL;
    SMARTLIST_FOREACH_BEGIN (s_values, const char *, cp) {
      const char *maxstr = NULL;
      v = tor_parse_uint64(cp, 10, 0, UINT64_MAX, &ok, NULL);
      if (have_maxima) {
        maxstr = smartlist_get(s_maxima, cp_sl_idx);
        mv = tor_parse_uint64(maxstr, 10, 0, UINT64_MAX, &ok_m, NULL);
        mv *= NUM_SECS_ROLLING_MEASURE;
      } else {
        /* No maxima known; guess average rate to be conservative. */
        mv = (v / s_interval) * NUM_SECS_ROLLING_MEASURE;
      }
      if (!ok) {
        retval = -1;
        log_notice(LD_HIST, "Could not parse value '%s' into a number.'", cp);
      }
      if (maxstr && !ok_m) {
        retval = -1;
        log_notice(LD_HIST, "Could not parse maximum '%s' into a number.'",
                   maxstr);
      }

      if (start < now) {
        time_t cur_start = start;
        time_t actual_interval_len = s_interval;
        uint64_t cur_val = 0;
        /* Calculate the average per second. This is the best we can do
         * because our state file doesn't have per-second resolution. */
        if (start + s_interval > now)
          actual_interval_len = now - start;
        cur_val = v / actual_interval_len;
        /* This is potentially inefficient, but since we don't do it very
         * often it should be ok. */
        while (cur_start < start + actual_interval_len) {
          add_obs(b, cur_start, cur_val);
          ++cur_start;
        }
        b->max_total = mv;
        /* This will result in some fairly choppy history if s_interval
         * is not the same as NUM_SECS_BW_SUM_INTERVAL. XXXX */
        start += actual_interval_len;
      }
    } SMARTLIST_FOREACH_END (cp);
  }

  /* Clean up maxima and observed */
  for (i = 0; i < NUM_SECS_ROLLING_MEASURE; ++i) {
    b->obs[i] = 0;
  }
  b->total_obs = 0;

  return retval;
}

/** Set bandwidth history from the state file we just loaded. */
int
rep_hist_load_state(or_state_t *state, char **err)
{
  int all_ok = 1;

  /* Assert they already have been malloced */
  tor_assert(read_array && write_array);
  tor_assert(dir_read_array && dir_write_array);

#define LOAD(arrname, st)                                           \
  if (rep_hist_load_bwhist_state_section(                           \
          (arrname), state->BWHistory##st##Values,                  \
          state->BWHistory##st##Maxima, state->BWHistory##st##Ends, \
          state->BWHistory##st##Interval) < 0)                      \
  all_ok = 0

  LOAD(write_array, Write);
  LOAD(read_array, Read);
  LOAD(dir_write_array, DirWrite);
  LOAD(dir_read_array, DirRead);

#undef LOAD
  if (!all_ok) {
    *err = tor_strdup("Parsing of bandwidth history values failed");
    /* and create fresh arrays */
    bw_arrays_init();
    return -1;
  }
  return 0;
}

/*** Exit port statistics ***/

/* Some constants */
/** To what multiple should byte numbers be rounded up? */
#define EXIT_STATS_ROUND_UP_BYTES 1024
/** To what multiple should stream counts be rounded up? */
#define EXIT_STATS_ROUND_UP_STREAMS 4
/** Number of TCP ports */
#define EXIT_STATS_NUM_PORTS 65536
/** Top n ports that will be included in exit stats. */
#define EXIT_STATS_TOP_N_PORTS 10

/* The following data structures are arrays and no fancy smartlists or maps,
 * so that all write operations can be done in constant time. This comes at
 * the price of some memory (1.25 MB) and linear complexity when writing
 * stats for measuring relays. */
/** Number of bytes read in current period by exit port */
static uint64_t *exit_bytes_read = NULL;
/** Number of bytes written in current period by exit port */
static uint64_t *exit_bytes_written = NULL;
/** Number of streams opened in current period by exit port */
static uint32_t *exit_streams = NULL;

/** Start time of exit stats or 0 if we're not collecting exit stats. */
static time_t start_of_exit_stats_interval;

/** Initialize exit port stats. */
void
rep_hist_exit_stats_init(time_t now)
{
  start_of_exit_stats_interval = now;
  exit_bytes_read = tor_calloc(EXIT_STATS_NUM_PORTS, sizeof(uint64_t));
  exit_bytes_written = tor_calloc(EXIT_STATS_NUM_PORTS, sizeof(uint64_t));
  exit_streams = tor_calloc(EXIT_STATS_NUM_PORTS, sizeof(uint32_t));
}

/** Reset counters for exit port statistics. */
void
rep_hist_reset_exit_stats(time_t now)
{
  start_of_exit_stats_interval = now;
  memset(exit_bytes_read, 0, EXIT_STATS_NUM_PORTS * sizeof(uint64_t));
  memset(exit_bytes_written, 0, EXIT_STATS_NUM_PORTS * sizeof(uint64_t));
  memset(exit_streams, 0, EXIT_STATS_NUM_PORTS * sizeof(uint32_t));
}

/** Stop collecting exit port stats in a way that we can re-start doing
 * so in rep_hist_exit_stats_init(). */
void
rep_hist_exit_stats_term(void)
{
  start_of_exit_stats_interval = 0;
  tor_free(exit_bytes_read);
  tor_free(exit_bytes_written);
  tor_free(exit_streams);
}

/** Helper for qsort: compare two ints.  Does not handle overflow properly,
 * but works fine for sorting an array of port numbers, which is what we use
 * it for. */
static int
compare_int_(const void *x, const void *y)
{
  return (*(int *)x - *(int *)y);
}

/** Return a newly allocated string containing the exit port statistics
 * until <b>now</b>, or NULL if we're not collecting exit stats. Caller
 * must ensure start_of_exit_stats_interval is in the past. */
char *
rep_hist_format_exit_stats(time_t now)
{
  int i, j, top_elements = 0, cur_min_idx = 0, cur_port;
  uint64_t top_bytes[EXIT_STATS_TOP_N_PORTS];
  int top_ports[EXIT_STATS_TOP_N_PORTS];
  uint64_t cur_bytes = 0, other_read = 0, other_written = 0, total_read = 0,
           total_written = 0;
  uint32_t total_streams = 0, other_streams = 0;
  smartlist_t *written_strings, *read_strings, *streams_strings;
  char *written_string, *read_string, *streams_string;
  char t[ISO_TIME_LEN + 1];
  char *result;

  if (!start_of_exit_stats_interval)
    return NULL; /* Not initialized. */

  tor_assert(now >= start_of_exit_stats_interval);

  /* Go through all ports to find the n ports that saw most written and
   * read bytes.
   *
   * Invariant: at the end of the loop for iteration i,
   *    total_read is the sum of all exit_bytes_read[0..i]
   *    total_written is the sum of all exit_bytes_written[0..i]
   *    total_stream is the sum of all exit_streams[0..i]
   *
   *    top_elements = MAX(EXIT_STATS_TOP_N_PORTS,
   *                  #{j | 0 <= j <= i && volume(i) > 0})
   *
   *    For all 0 <= j < top_elements,
   *        top_bytes[j] > 0
   *        0 <= top_ports[j] <= 65535
   *        top_bytes[j] = volume(top_ports[j])
   *
   *    There is no j in 0..i and k in 0..top_elements such that:
   *        volume(j) > top_bytes[k] AND j is not in top_ports[0..top_elements]
   *
   *    There is no j!=cur_min_idx in 0..top_elements such that:
   *        top_bytes[j] < top_bytes[cur_min_idx]
   *
   * where volume(x) == exit_bytes_read[x]+exit_bytes_written[x]
   *
   * Worst case: O(EXIT_STATS_NUM_PORTS * EXIT_STATS_TOP_N_PORTS)
   */
  for (i = 1; i < EXIT_STATS_NUM_PORTS; i++) {
    total_read += exit_bytes_read[i];
    total_written += exit_bytes_written[i];
    total_streams += exit_streams[i];
    cur_bytes = exit_bytes_read[i] + exit_bytes_written[i];
    if (cur_bytes == 0) {
      continue;
    }
    if (top_elements < EXIT_STATS_TOP_N_PORTS) {
      top_bytes[top_elements] = cur_bytes;
      top_ports[top_elements++] = i;
    } else if (cur_bytes > top_bytes[cur_min_idx]) {
      top_bytes[cur_min_idx] = cur_bytes;
      top_ports[cur_min_idx] = i;
    } else {
      continue;
    }
    cur_min_idx = 0;
    for (j = 1; j < top_elements; j++) {
      if (top_bytes[j] < top_bytes[cur_min_idx]) {
        cur_min_idx = j;
      }
    }
  }

  /* Add observations of top ports to smartlists. */
  written_strings = smartlist_new();
  read_strings = smartlist_new();
  streams_strings = smartlist_new();
  other_read = total_read;
  other_written = total_written;
  other_streams = total_streams;
  /* Sort the ports; this puts them out of sync with top_bytes, but we
   * won't be using top_bytes again anyway */
  qsort(top_ports, top_elements, sizeof(int), compare_int_);
  for (j = 0; j < top_elements; j++) {
    cur_port = top_ports[j];
    if (exit_bytes_written[cur_port] > 0) {
      uint64_t num = round_uint64_to_next_multiple_of(
          exit_bytes_written[cur_port], EXIT_STATS_ROUND_UP_BYTES);
      num /= 1024;
      smartlist_add_asprintf(written_strings, "%d=%" PRIu64, cur_port, (num));
      other_written -= exit_bytes_written[cur_port];
    }
    if (exit_bytes_read[cur_port] > 0) {
      uint64_t num = round_uint64_to_next_multiple_of(
          exit_bytes_read[cur_port], EXIT_STATS_ROUND_UP_BYTES);
      num /= 1024;
      smartlist_add_asprintf(read_strings, "%d=%" PRIu64, cur_port, (num));
      other_read -= exit_bytes_read[cur_port];
    }
    if (exit_streams[cur_port] > 0) {
      uint32_t num = round_uint32_to_next_multiple_of(
          exit_streams[cur_port], EXIT_STATS_ROUND_UP_STREAMS);
      smartlist_add_asprintf(streams_strings, "%d=%u", cur_port, num);
      other_streams -= exit_streams[cur_port];
    }
  }

  /* Add observations of other ports in a single element. */
  other_written = round_uint64_to_next_multiple_of(other_written,
                                                   EXIT_STATS_ROUND_UP_BYTES);
  other_written /= 1024;
  smartlist_add_asprintf(written_strings, "other=%" PRIu64, (other_written));
  other_read =
      round_uint64_to_next_multiple_of(other_read, EXIT_STATS_ROUND_UP_BYTES);
  other_read /= 1024;
  smartlist_add_asprintf(read_strings, "other=%" PRIu64, (other_read));
  other_streams = round_uint32_to_next_multiple_of(
      other_streams, EXIT_STATS_ROUND_UP_STREAMS);
  smartlist_add_asprintf(streams_strings, "other=%u", other_streams);

  /* Join all observations in single strings. */
  written_string = smartlist_join_strings(written_strings, ",", 0, NULL);
  read_string = smartlist_join_strings(read_strings, ",", 0, NULL);
  streams_string = smartlist_join_strings(streams_strings, ",", 0, NULL);
  SMARTLIST_FOREACH(written_strings, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(read_strings, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(streams_strings, char *, cp, tor_free(cp));
  smartlist_free(written_strings);
  smartlist_free(read_strings);
  smartlist_free(streams_strings);

  /* Put everything together. */
  format_iso_time(t, now);
  tor_asprintf(&result,
               "exit-stats-end %s (%d s)\n"
               "exit-kibibytes-written %s\n"
               "exit-kibibytes-read %s\n"
               "exit-streams-opened %s\n",
               t, (unsigned)(now - start_of_exit_stats_interval),
               written_string, read_string, streams_string);
  tor_free(written_string);
  tor_free(read_string);
  tor_free(streams_string);
  return result;
}

/** If 24 hours have passed since the beginning of the current exit port
 * stats period, write exit stats to $DATADIR/stats/exit-stats (possibly
 * overwriting an existing file) and reset counters.  Return when we would
 * next want to write exit stats or 0 if we never want to write. */
time_t
rep_hist_exit_stats_write(time_t now)
{
  char *str = NULL;

  if (!start_of_exit_stats_interval)
    return 0; /* Not initialized. */
  if (start_of_exit_stats_interval + WRITE_STATS_INTERVAL > now)
    goto done; /* Not ready to write. */

  log_info(LD_HIST, "Writing exit port statistics to disk.");

  /* Generate history string. */
  str = rep_hist_format_exit_stats(now);

  /* Reset counters. */
  rep_hist_reset_exit_stats(now);

  /* Try to write to disk. */
  if (!check_or_create_data_subdir("stats")) {
    write_to_data_subdir("stats", "exit-stats", str, "exit port statistics");
  }

done:
  tor_free(str);
  return start_of_exit_stats_interval + WRITE_STATS_INTERVAL;
}

/** Note that we wrote <b>num_written</b> bytes and read <b>num_read</b>
 * bytes to/from an exit connection to <b>port</b>. */
void
rep_hist_note_exit_bytes(uint16_t port, size_t num_written, size_t num_read)
{
  if (!start_of_exit_stats_interval)
    return; /* Not initialized. */
  exit_bytes_written[port] += num_written;
  exit_bytes_read[port] += num_read;
  log_debug(LD_HIST,
            "Written %lu bytes and read %lu bytes to/from an "
            "exit connection to port %d.",
            (unsigned long)num_written, (unsigned long)num_read, port);
}

/** Note that we opened an exit stream to <b>port</b>. */
void
rep_hist_note_exit_stream_opened(uint16_t port)
{
  if (!start_of_exit_stats_interval)
    return; /* Not initialized. */
  exit_streams[port]++;
  log_debug(LD_HIST, "Opened exit stream to port %d", port);
}

/*** cell statistics ***/

/** Start of the current buffer stats interval or 0 if we're not
 * collecting buffer statistics. */
static time_t start_of_buffer_stats_interval;

/** Initialize buffer stats. */
void
rep_hist_buffer_stats_init(time_t now)
{
  start_of_buffer_stats_interval = now;
}

/** Statistics from a single circuit.  Collected when the circuit closes, or
 * when we flush statistics to disk. */
typedef struct circ_buffer_stats_t {
  /** Average number of cells in the circuit's queue */
  double mean_num_cells_in_queue;
  /** Average time a cell waits in the queue. */
  double mean_time_cells_in_queue;
  /** Total number of cells sent over this circuit */
  uint32_t processed_cells;
} circ_buffer_stats_t;

/** List of circ_buffer_stats_t. */
static smartlist_t *circuits_for_buffer_stats = NULL;

/** Remember cell statistics <b>mean_num_cells_in_queue</b>,
 * <b>mean_time_cells_in_queue</b>, and <b>processed_cells</b> of a
 * circuit. */
void
rep_hist_add_buffer_stats(double mean_num_cells_in_queue,
                          double mean_time_cells_in_queue,
                          uint32_t processed_cells)
{
  circ_buffer_stats_t *stats;
  if (!start_of_buffer_stats_interval)
    return; /* Not initialized. */
  stats = tor_malloc_zero(sizeof(circ_buffer_stats_t));
  stats->mean_num_cells_in_queue = mean_num_cells_in_queue;
  stats->mean_time_cells_in_queue = mean_time_cells_in_queue;
  stats->processed_cells = processed_cells;
  if (!circuits_for_buffer_stats)
    circuits_for_buffer_stats = smartlist_new();
  smartlist_add(circuits_for_buffer_stats, stats);
}

/** Remember cell statistics for circuit <b>circ</b> at time
 * <b>end_of_interval</b> and reset cell counters in case the circuit
 * remains open in the next measurement interval. */
void
rep_hist_buffer_stats_add_circ(circuit_t *circ, time_t end_of_interval)
{
  time_t start_of_interval;
  int interval_length;
  or_circuit_t *orcirc;
  double mean_num_cells_in_queue, mean_time_cells_in_queue;
  uint32_t processed_cells;
  if (CIRCUIT_IS_ORIGIN(circ))
    return;
  orcirc = TO_OR_CIRCUIT(circ);
  if (!orcirc->processed_cells)
    return;
  start_of_interval =
      (circ->timestamp_created.tv_sec > start_of_buffer_stats_interval)
          ? (time_t)circ->timestamp_created.tv_sec
          : start_of_buffer_stats_interval;
  interval_length = (int)(end_of_interval - start_of_interval);
  if (interval_length <= 0)
    return;
  processed_cells = orcirc->processed_cells;
  /* 1000.0 for s -> ms; 2.0 because of app-ward and exit-ward queues */
  mean_num_cells_in_queue = (double)orcirc->total_cell_waiting_time /
                            (double)interval_length / 1000.0 / 2.0;
  mean_time_cells_in_queue = (double)orcirc->total_cell_waiting_time /
                             (double)orcirc->processed_cells;
  orcirc->total_cell_waiting_time = 0;
  orcirc->processed_cells = 0;
  rep_hist_add_buffer_stats(mean_num_cells_in_queue, mean_time_cells_in_queue,
                            processed_cells);
}

/** Sorting helper: return -1, 1, or 0 based on comparison of two
 * circ_buffer_stats_t */
static int
buffer_stats_compare_entries_(const void **_a, const void **_b)
{
  const circ_buffer_stats_t *a = *_a, *b = *_b;
  if (a->processed_cells < b->processed_cells)
    return 1;
  else if (a->processed_cells > b->processed_cells)
    return -1;
  else
    return 0;
}

/** Stop collecting cell stats in a way that we can re-start doing so in
 * rep_hist_buffer_stats_init(). */
void
rep_hist_buffer_stats_term(void)
{
  rep_hist_reset_buffer_stats(0);
}

/** Clear history of circuit statistics and set the measurement interval
 * start to <b>now</b>. */
void
rep_hist_reset_buffer_stats(time_t now)
{
  if (!circuits_for_buffer_stats)
    circuits_for_buffer_stats = smartlist_new();
  SMARTLIST_FOREACH(circuits_for_buffer_stats, circ_buffer_stats_t *, stats,
                    tor_free(stats));
  smartlist_clear(circuits_for_buffer_stats);
  start_of_buffer_stats_interval = now;
}

/** Return a newly allocated string containing the buffer statistics until
 * <b>now</b>, or NULL if we're not collecting buffer stats. Caller must
 * ensure start_of_buffer_stats_interval is in the past. */
char *
rep_hist_format_buffer_stats(time_t now)
{
#define SHARES 10
  uint64_t processed_cells[SHARES];
  uint32_t circs_in_share[SHARES];
  int number_of_circuits, i;
  double queued_cells[SHARES], time_in_queue[SHARES];
  smartlist_t *processed_cells_strings, *queued_cells_strings,
      *time_in_queue_strings;
  char *processed_cells_string, *queued_cells_string, *time_in_queue_string;
  char t[ISO_TIME_LEN + 1];
  char *result;

  if (!start_of_buffer_stats_interval)
    return NULL; /* Not initialized. */

  tor_assert(now >= start_of_buffer_stats_interval);

  /* Calculate deciles if we saw at least one circuit. */
  memset(processed_cells, 0, SHARES * sizeof(uint64_t));
  memset(circs_in_share, 0, SHARES * sizeof(uint32_t));
  memset(queued_cells, 0, SHARES * sizeof(double));
  memset(time_in_queue, 0, SHARES * sizeof(double));
  if (!circuits_for_buffer_stats)
    circuits_for_buffer_stats = smartlist_new();
  number_of_circuits = smartlist_len(circuits_for_buffer_stats);
  if (number_of_circuits > 0) {
    smartlist_sort(circuits_for_buffer_stats, buffer_stats_compare_entries_);
    i = 0;
    SMARTLIST_FOREACH_BEGIN (circuits_for_buffer_stats, circ_buffer_stats_t *,
                             stats) {
      int share = i++ * SHARES / number_of_circuits;
      processed_cells[share] += stats->processed_cells;
      queued_cells[share] += stats->mean_num_cells_in_queue;
      time_in_queue[share] += stats->mean_time_cells_in_queue;
      circs_in_share[share]++;
    } SMARTLIST_FOREACH_END (stats);
  }

  /* Write deciles to strings. */
  processed_cells_strings = smartlist_new();
  queued_cells_strings = smartlist_new();
  time_in_queue_strings = smartlist_new();
  for (i = 0; i < SHARES; i++) {
    smartlist_add_asprintf(
        processed_cells_strings, "%" PRIu64,
        !circs_in_share[i] ? 0 : (processed_cells[i] / circs_in_share[i]));
  }
  for (i = 0; i < SHARES; i++) {
    smartlist_add_asprintf(queued_cells_strings, "%.2f",
                           circs_in_share[i] == 0
                               ? 0.0
                               : queued_cells[i] / (double)circs_in_share[i]);
  }
  for (i = 0; i < SHARES; i++) {
    smartlist_add_asprintf(time_in_queue_strings, "%.0f",
                           circs_in_share[i] == 0
                               ? 0.0
                               : time_in_queue[i] / (double)circs_in_share[i]);
  }

  /* Join all observations in single strings. */
  processed_cells_string =
      smartlist_join_strings(processed_cells_strings, ",", 0, NULL);
  queued_cells_string =
      smartlist_join_strings(queued_cells_strings, ",", 0, NULL);
  time_in_queue_string =
      smartlist_join_strings(time_in_queue_strings, ",", 0, NULL);
  SMARTLIST_FOREACH(processed_cells_strings, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(queued_cells_strings, char *, cp, tor_free(cp));
  SMARTLIST_FOREACH(time_in_queue_strings, char *, cp, tor_free(cp));
  smartlist_free(processed_cells_strings);
  smartlist_free(queued_cells_strings);
  smartlist_free(time_in_queue_strings);

  /* Put everything together. */
  format_iso_time(t, now);
  tor_asprintf(&result,
               "cell-stats-end %s (%d s)\n"
               "cell-processed-cells %s\n"
               "cell-queued-cells %s\n"
               "cell-time-in-queue %s\n"
               "cell-circuits-per-decile %d\n",
               t, (unsigned)(now - start_of_buffer_stats_interval),
               processed_cells_string, queued_cells_string,
               time_in_queue_string, CEIL_DIV(number_of_circuits, SHARES));
  tor_free(processed_cells_string);
  tor_free(queued_cells_string);
  tor_free(time_in_queue_string);
  return result;
#undef SHARES
}

/** If 24 hours have passed since the beginning of the current buffer
 * stats period, write buffer stats to $DATADIR/stats/buffer-stats
 * (possibly overwriting an existing file) and reset counters.  Return
 * when we would next want to write buffer stats or 0 if we never want to
 * write. */
time_t
rep_hist_buffer_stats_write(time_t now)
{
  char *str = NULL;

  if (!start_of_buffer_stats_interval)
    return 0; /* Not initialized. */
  if (start_of_buffer_stats_interval + WRITE_STATS_INTERVAL > now)
    goto done; /* Not ready to write */

  /* Add open circuits to the history. */
  SMARTLIST_FOREACH_BEGIN (circuit_get_global_list(), circuit_t *, circ) {
    rep_hist_buffer_stats_add_circ(circ, now);
  } SMARTLIST_FOREACH_END (circ);

  /* Generate history string. */
  str = rep_hist_format_buffer_stats(now);

  /* Reset both buffer history and counters of open circuits. */
  rep_hist_reset_buffer_stats(now);

  /* Try to write to disk. */
  if (!check_or_create_data_subdir("stats")) {
    write_to_data_subdir("stats", "buffer-stats", str, "buffer statistics");
  }

done:
  tor_free(str);
  return start_of_buffer_stats_interval + WRITE_STATS_INTERVAL;
}

/*** Descriptor serving statistics ***/

/** Digestmap to track which descriptors were downloaded this stats
 *  collection interval. It maps descriptor digest to pointers to 1,
 *  effectively turning this into a list. */
static digestmap_t *served_descs = NULL;

/** Number of how many descriptors were downloaded in total during this
 * interval. */
static unsigned long total_descriptor_downloads;

/** Start time of served descs stats or 0 if we're not collecting those. */
static time_t start_of_served_descs_stats_interval;

/** Initialize descriptor stats. */
void
rep_hist_desc_stats_init(time_t now)
{
  if (served_descs) {
    log_warn(LD_BUG, "Called rep_hist_desc_stats_init() when desc stats were "
                     "already initialized. This is probably harmless.");
    return; // Already initialized
  }
  served_descs = digestmap_new();
  total_descriptor_downloads = 0;
  start_of_served_descs_stats_interval = now;
}

/** Reset served descs stats to empty, starting a new interval <b>now</b>. */
static void
rep_hist_reset_desc_stats(time_t now)
{
  rep_hist_desc_stats_term();
  rep_hist_desc_stats_init(now);
}

/** Stop collecting served descs stats, so that rep_hist_desc_stats_init() is
 * safe to be called again. */
void
rep_hist_desc_stats_term(void)
{
  digestmap_free(served_descs, NULL);
  served_descs = NULL;
  start_of_served_descs_stats_interval = 0;
  total_descriptor_downloads = 0;
}

/** Helper for rep_hist_desc_stats_write(). Return a newly allocated string
 * containing the served desc statistics until now, or NULL if we're not
 * collecting served desc stats. Caller must ensure that now is not before
 * start_of_served_descs_stats_interval. */
static char *
rep_hist_format_desc_stats(time_t now)
{
  char t[ISO_TIME_LEN + 1];
  char *result;

  digestmap_iter_t *iter;
  const char *key;
  void *val;
  unsigned size;
  int *vals, max = 0, q3 = 0, md = 0, q1 = 0, min = 0;
  int n = 0;

  if (!start_of_served_descs_stats_interval)
    return NULL;

  size = digestmap_size(served_descs);
  if (size > 0) {
    vals = tor_calloc(size, sizeof(int));
    for (iter = digestmap_iter_init(served_descs); !digestmap_iter_done(iter);
         iter = digestmap_iter_next(served_descs, iter)) {
      uintptr_t count;
      digestmap_iter_get(iter, &key, &val);
      count = (uintptr_t)val;
      vals[n++] = (int)count;
      (void)key;
    }
    max = find_nth_int(vals, size, size - 1);
    q3 = find_nth_int(vals, size, (3 * size - 1) / 4);
    md = find_nth_int(vals, size, (size - 1) / 2);
    q1 = find_nth_int(vals, size, (size - 1) / 4);
    min = find_nth_int(vals, size, 0);
    tor_free(vals);
  }

  format_iso_time(t, now);

  tor_asprintf(&result,
               "served-descs-stats-end %s (%d s) total=%lu unique=%u "
               "max=%d q3=%d md=%d q1=%d min=%d\n",
               t, (unsigned)(now - start_of_served_descs_stats_interval),
               total_descriptor_downloads, size, max, q3, md, q1, min);

  return result;
}

/** If WRITE_STATS_INTERVAL seconds have passed since the beginning of
 * the current served desc stats interval, write the stats to
 * $DATADIR/stats/served-desc-stats (possibly appending to an existing file)
 * and reset the state for the next interval. Return when we would next want
 * to write served desc stats or 0 if we won't want to write. */
time_t
rep_hist_desc_stats_write(time_t now)
{
  char *filename = NULL, *str = NULL;

  if (!start_of_served_descs_stats_interval)
    return 0; /* We're not collecting stats. */
  if (start_of_served_descs_stats_interval + WRITE_STATS_INTERVAL > now)
    return start_of_served_descs_stats_interval + WRITE_STATS_INTERVAL;

  str = rep_hist_format_desc_stats(now);
  tor_assert(str != NULL);

  if (check_or_create_data_subdir("stats") < 0) {
    goto done;
  }
  filename = get_datadir_fname2("stats", "served-desc-stats");
  if (append_bytes_to_file(filename, str, strlen(str), 0) < 0)
    log_warn(LD_HIST, "Unable to write served descs statistics to disk!");

  rep_hist_reset_desc_stats(now);

done:
  tor_free(filename);
  tor_free(str);
  return start_of_served_descs_stats_interval + WRITE_STATS_INTERVAL;
}

/** Called to note that we've served a given descriptor (by
 * digest). Increments the count of descriptors served, and the number
 * of times we've served this descriptor. */
void
rep_hist_note_desc_served(const char *desc)
{
  void *val;
  uintptr_t count;
  if (!served_descs)
    return; // We're not collecting stats
  val = digestmap_get(served_descs, desc);
  count = (uintptr_t)val;
  if (count != INT_MAX)
    ++count;
  digestmap_set(served_descs, desc, (void *)count);
  total_descriptor_downloads++;
}

/*** Connection statistics ***/

/** Start of the current connection stats interval or 0 if we're not
 * collecting connection statistics. */
static time_t start_of_conn_stats_interval;

/** Initialize connection stats. */
void
rep_hist_conn_stats_init(time_t now)
{
  start_of_conn_stats_interval = now;
}

/* Count connections that we read and wrote less than these many bytes
 * from/to as below threshold. */
#define BIDI_THRESHOLD 20480

/* Count connections that we read or wrote at least this factor as many
 * bytes from/to than we wrote or read to/from as mostly reading or
 * writing. */
#define BIDI_FACTOR 10

/* Interval length in seconds for considering read and written bytes for
 * connection stats. */
#define BIDI_INTERVAL 10

/** Start of next BIDI_INTERVAL second interval. */
static time_t bidi_next_interval = 0;

/** Number of connections that we read and wrote less than BIDI_THRESHOLD
 * bytes from/to in BIDI_INTERVAL seconds. */
static uint32_t below_threshold = 0;

/** Number of connections that we read at least BIDI_FACTOR times more
 * bytes from than we wrote to in BIDI_INTERVAL seconds. */
static uint32_t mostly_read = 0;

/** Number of connections that we wrote at least BIDI_FACTOR times more
 * bytes to than we read from in BIDI_INTERVAL seconds. */
static uint32_t mostly_written = 0;

/** Number of connections that we read and wrote at least BIDI_THRESHOLD
 * bytes from/to, but not BIDI_FACTOR times more in either direction in
 * BIDI_INTERVAL seconds. */
static uint32_t both_read_and_written = 0;

/** Entry in a map from connection ID to the number of read and written
 * bytes on this connection in a BIDI_INTERVAL second interval. */
typedef struct bidi_map_entry_t {
  HT_ENTRY(bidi_map_entry_t) node;
  uint64_t conn_id; /**< Connection ID */
  size_t read; /**< Number of read bytes */
  size_t written; /**< Number of written bytes */
} bidi_map_entry_t;

/** Map of OR connections together with the number of read and written
 * bytes in the current BIDI_INTERVAL second interval. */
static HT_HEAD(bidimap, bidi_map_entry_t) bidi_map = HT_INITIALIZER();

static int
bidi_map_ent_eq(const bidi_map_entry_t *a, const bidi_map_entry_t *b)
{
  return a->conn_id == b->conn_id;
}

/* DOCDOC bidi_map_ent_hash */
static unsigned
bidi_map_ent_hash(const bidi_map_entry_t *entry)
{
  return (unsigned)entry->conn_id;
}

HT_PROTOTYPE(bidimap, bidi_map_entry_t, node, bidi_map_ent_hash,
             bidi_map_ent_eq);
HT_GENERATE2(bidimap, bidi_map_entry_t, node, bidi_map_ent_hash,
             bidi_map_ent_eq, 0.6, tor_reallocarray_, tor_free_);

/* DOCDOC bidi_map_free */
static void
bidi_map_free_all(void)
{
  bidi_map_entry_t **ptr, **next, *ent;
  for (ptr = HT_START(bidimap, &bidi_map); ptr; ptr = next) {
    ent = *ptr;
    next = HT_NEXT_RMV(bidimap, &bidi_map, ptr);
    tor_free(ent);
  }
  HT_CLEAR(bidimap, &bidi_map);
}

/** Reset counters for conn statistics. */
void
rep_hist_reset_conn_stats(time_t now)
{
  start_of_conn_stats_interval = now;
  below_threshold = 0;
  mostly_read = 0;
  mostly_written = 0;
  both_read_and_written = 0;
  bidi_map_free_all();
}

/** Stop collecting connection stats in a way that we can re-start doing
 * so in rep_hist_conn_stats_init(). */
void
rep_hist_conn_stats_term(void)
{
  rep_hist_reset_conn_stats(0);
}

/** We read <b>num_read</b> bytes and wrote <b>num_written</b> from/to OR
 * connection <b>conn_id</b> in second <b>when</b>. If this is the first
 * observation in a new interval, sum up the last observations. Add bytes
 * for this connection. */
void
rep_hist_note_or_conn_bytes(uint64_t conn_id, size_t num_read,
                            size_t num_written, time_t when)
{
  if (!start_of_conn_stats_interval)
    return;
  /* Initialize */
  if (bidi_next_interval == 0)
    bidi_next_interval = when + BIDI_INTERVAL;
  /* Sum up last period's statistics */
  if (when >= bidi_next_interval) {
    bidi_map_entry_t **ptr, **next, *ent;
    for (ptr = HT_START(bidimap, &bidi_map); ptr; ptr = next) {
      ent = *ptr;
      if (ent->read + ent->written < BIDI_THRESHOLD)
        below_threshold++;
      else if (ent->read >= ent->written * BIDI_FACTOR)
        mostly_read++;
      else if (ent->written >= ent->read * BIDI_FACTOR)
        mostly_written++;
      else
        both_read_and_written++;
      next = HT_NEXT_RMV(bidimap, &bidi_map, ptr);
      tor_free(ent);
    }
    while (when >= bidi_next_interval)
      bidi_next_interval += BIDI_INTERVAL;
    log_info(LD_GENERAL,
             "%d below threshold, %d mostly read, "
             "%d mostly written, %d both read and written.",
             below_threshold, mostly_read, mostly_written,
             both_read_and_written);
  }
  /* Add this connection's bytes. */
  if (num_read > 0 || num_written > 0) {
    bidi_map_entry_t *entry, lookup;
    lookup.conn_id = conn_id;
    entry = HT_FIND(bidimap, &bidi_map, &lookup);
    if (entry) {
      entry->written += num_written;
      entry->read += num_read;
    } else {
      entry = tor_malloc_zero(sizeof(bidi_map_entry_t));
      entry->conn_id = conn_id;
      entry->written = num_written;
      entry->read = num_read;
      HT_INSERT(bidimap, &bidi_map, entry);
    }
  }
}

/** Return a newly allocated string containing the connection statistics
 * until <b>now</b>, or NULL if we're not collecting conn stats. Caller must
 * ensure start_of_conn_stats_interval is in the past. */
char *
rep_hist_format_conn_stats(time_t now)
{
  char *result, written[ISO_TIME_LEN + 1];

  if (!start_of_conn_stats_interval)
    return NULL; /* Not initialized. */

  tor_assert(now >= start_of_conn_stats_interval);

  format_iso_time(written, now);
  tor_asprintf(&result, "conn-bi-direct %s (%d s) %d,%d,%d,%d\n", written,
               (unsigned)(now - start_of_conn_stats_interval), below_threshold,
               mostly_read, mostly_written, both_read_and_written);
  return result;
}

/** If 24 hours have passed since the beginning of the current conn stats
 * period, write conn stats to $DATADIR/stats/conn-stats (possibly
 * overwriting an existing file) and reset counters.  Return when we would
 * next want to write conn stats or 0 if we never want to write. */
time_t
rep_hist_conn_stats_write(time_t now)
{
  char *str = NULL;

  if (!start_of_conn_stats_interval)
    return 0; /* Not initialized. */
  if (start_of_conn_stats_interval + WRITE_STATS_INTERVAL > now)
    goto done; /* Not ready to write */

  /* Generate history string. */
  str = rep_hist_format_conn_stats(now);

  /* Reset counters. */
  rep_hist_reset_conn_stats(now);

  /* Try to write to disk. */
  if (!check_or_create_data_subdir("stats")) {
    write_to_data_subdir("stats", "conn-stats", str, "connection statistics");
  }

done:
  tor_free(str);
  return start_of_conn_stats_interval + WRITE_STATS_INTERVAL;
}

/** Internal statistics to track how many requests of each type of
 * handshake we've received, and how many we've assigned to cpuworkers.
 * Useful for seeing trends in cpu load.
 * @{ */
STATIC int onion_handshakes_requested[MAX_ONION_HANDSHAKE_TYPE + 1] = {0};
STATIC int onion_handshakes_assigned[MAX_ONION_HANDSHAKE_TYPE + 1] = {0};
/**@}*/

/** A new onionskin (using the <b>type</b> handshake) has arrived. */
void
rep_hist_note_circuit_handshake_requested(uint16_t type)
{
  if (type <= MAX_ONION_HANDSHAKE_TYPE)
    onion_handshakes_requested[type]++;
}

/** We've sent an onionskin (using the <b>type</b> handshake) to a
 * cpuworker. */
void
rep_hist_note_circuit_handshake_assigned(uint16_t type)
{
  if (type <= MAX_ONION_HANDSHAKE_TYPE)
    onion_handshakes_assigned[type]++;
}

/** Log our onionskin statistics since the last time we were called. */
void
rep_hist_log_circuit_handshake_stats(time_t now)
{
  (void)now;
  log_notice(LD_HEARTBEAT,
             "Circuit handshake stats since last time: "
             "%d/%d TAP, %d/%d NTor.",
             onion_handshakes_assigned[ONION_HANDSHAKE_TYPE_TAP],
             onion_handshakes_requested[ONION_HANDSHAKE_TYPE_TAP],
             onion_handshakes_assigned[ONION_HANDSHAKE_TYPE_NTOR],
             onion_handshakes_requested[ONION_HANDSHAKE_TYPE_NTOR]);
  memset(onion_handshakes_assigned, 0, sizeof(onion_handshakes_assigned));
  memset(onion_handshakes_requested, 0, sizeof(onion_handshakes_requested));
}

/* Hidden service statistics section */

/** Start of the current hidden service stats interval or 0 if we're
 * not collecting hidden service statistics. */
static time_t start_of_hs_stats_interval;

/** Carries the various hidden service statistics, and any other
 *  information needed. */
typedef struct hs_stats_t {
  /** How many relay cells have we seen as rendezvous points? */
  uint64_t rp_relay_cells_seen;

  /** Set of unique public key digests we've seen this stat period
   * (could also be implemented as sorted smartlist). */
  digestmap_t *onions_seen_this_period;
} hs_stats_t;

/** Our statistics structure singleton. */
static hs_stats_t *hs_stats = NULL;

/** Allocate, initialize and return an hs_stats_t structure. */
static hs_stats_t *
hs_stats_new(void)
{
  hs_stats_t *new_hs_stats = tor_malloc_zero(sizeof(hs_stats_t));
  new_hs_stats->onions_seen_this_period = digestmap_new();

  return new_hs_stats;
}

#define hs_stats_free(val) FREE_AND_NULL(hs_stats_t, hs_stats_free_, (val))

/** Free an hs_stats_t structure. */
static void
hs_stats_free_(hs_stats_t *victim_hs_stats)
{
  if (!victim_hs_stats) {
    return;
  }

  digestmap_free(victim_hs_stats->onions_seen_this_period, NULL);
  tor_free(victim_hs_stats);
}

/** Initialize hidden service statistics. */
void
rep_hist_hs_stats_init(time_t now)
{
  if (!hs_stats) {
    hs_stats = hs_stats_new();
  }

  start_of_hs_stats_interval = now;
}

/** Clear history of hidden service statistics and set the measurement
 * interval start to <b>now</b>. */
static void
rep_hist_reset_hs_stats(time_t now)
{
  if (!hs_stats) {
    hs_stats = hs_stats_new();
  }

  hs_stats->rp_relay_cells_seen = 0;

  digestmap_free(hs_stats->onions_seen_this_period, NULL);
  hs_stats->onions_seen_this_period = digestmap_new();

  start_of_hs_stats_interval = now;
}

/** Stop collecting hidden service stats in a way that we can re-start
 * doing so in rep_hist_buffer_stats_init(). */
void
rep_hist_hs_stats_term(void)
{
  rep_hist_reset_hs_stats(0);
}

/** We saw a new HS relay cell, Count it! */
void
rep_hist_seen_new_rp_cell(void)
{
  if (!hs_stats) {
    return; // We're not collecting stats
  }

  hs_stats->rp_relay_cells_seen++;
}

/** As HSDirs, we saw another hidden service with public key
 *  <b>pubkey</b>. Check whether we have counted it before, if not
 *  count it now! */
void
rep_hist_stored_maybe_new_hs(const crypto_pk_t *pubkey)
{
  char pubkey_hash[DIGEST_LEN];

  if (!hs_stats) {
    return; // We're not collecting stats
  }

  /* Get the digest of the pubkey which will be used to detect whether
     we've seen this hidden service before or not.  */
  if (crypto_pk_get_digest(pubkey, pubkey_hash) < 0) {
    /*  This fail should not happen; key has been validated by
        descriptor parsing code first. */
    return;
  }

  /* Check if this is the first time we've seen this hidden
     service. If it is, count it as new. */
  if (!digestmap_get(hs_stats->onions_seen_this_period, pubkey_hash)) {
    digestmap_set(hs_stats->onions_seen_this_period, pubkey_hash,
                  (void *)(uintptr_t)1);
  }
}

/* The number of cells that are supposed to be hidden from the adversary
 * by adding noise from the Laplace distribution.  This value, divided by
 * EPSILON, is Laplace parameter b. It must be greather than 0. */
#define REND_CELLS_DELTA_F 2048
/* Security parameter for obfuscating number of cells with a value between
 * ]0.0, 1.0]. Smaller values obfuscate observations more, but at the same
 * time make statistics less usable. */
#define REND_CELLS_EPSILON 0.3
/* The number of cells that are supposed to be hidden from the adversary
 * by rounding up to the next multiple of this number. */
#define REND_CELLS_BIN_SIZE 1024
/* The number of service identities that are supposed to be hidden from the
 * adversary by adding noise from the Laplace distribution. This value,
 * divided by EPSILON, is Laplace parameter b. It must be greater than 0. */
#define ONIONS_SEEN_DELTA_F 8
/* Security parameter for obfuscating number of service identities with a
 * value between ]0.0, 1.0]. Smaller values obfuscate observations more, but
 * at the same time make statistics less usable. */
#define ONIONS_SEEN_EPSILON 0.3
/* The number of service identities that are supposed to be hidden from
 * the adversary by rounding up to the next multiple of this number. */
#define ONIONS_SEEN_BIN_SIZE 8

/** Allocate and return a string containing hidden service stats that
 *  are meant to be placed in the extra-info descriptor. */
static char *
rep_hist_format_hs_stats(time_t now)
{
  char t[ISO_TIME_LEN + 1];
  char *hs_stats_string;
  int64_t obfuscated_cells_seen;
  int64_t obfuscated_onions_seen;

  uint64_t rounded_cells_seen = round_uint64_to_next_multiple_of(
      hs_stats->rp_relay_cells_seen, REND_CELLS_BIN_SIZE);
  rounded_cells_seen = MIN(rounded_cells_seen, INT64_MAX);
  obfuscated_cells_seen =
      add_laplace_noise((int64_t)rounded_cells_seen, crypto_rand_double(),
                        REND_CELLS_DELTA_F, REND_CELLS_EPSILON);

  uint64_t rounded_onions_seen = round_uint64_to_next_multiple_of(
      (size_t)digestmap_size(hs_stats->onions_seen_this_period),
      ONIONS_SEEN_BIN_SIZE);
  rounded_onions_seen = MIN(rounded_onions_seen, INT64_MAX);
  obfuscated_onions_seen =
      add_laplace_noise((int64_t)rounded_onions_seen, crypto_rand_double(),
                        ONIONS_SEEN_DELTA_F, ONIONS_SEEN_EPSILON);

  format_iso_time(t, now);
  tor_asprintf(&hs_stats_string,
               "hidserv-stats-end %s (%d s)\n"
               "hidserv-rend-relayed-cells %" PRId64 " delta_f=%d "
               "epsilon=%.2f bin_size=%d\n"
               "hidserv-dir-onions-seen %" PRId64 " delta_f=%d "
               "epsilon=%.2f bin_size=%d\n",
               t, (unsigned)(now - start_of_hs_stats_interval),
               (obfuscated_cells_seen), REND_CELLS_DELTA_F, REND_CELLS_EPSILON,
               REND_CELLS_BIN_SIZE, (obfuscated_onions_seen),
               ONIONS_SEEN_DELTA_F, ONIONS_SEEN_EPSILON, ONIONS_SEEN_BIN_SIZE);

  return hs_stats_string;
}

/** If 24 hours have passed since the beginning of the current HS
 * stats period, write buffer stats to $DATADIR/stats/hidserv-stats
 * (possibly overwriting an existing file) and reset counters.  Return
 * when we would next want to write buffer stats or 0 if we never want to
 * write. */
time_t
rep_hist_hs_stats_write(time_t now)
{
  char *str = NULL;

  if (!start_of_hs_stats_interval) {
    return 0; /* Not initialized. */
  }

  if (start_of_hs_stats_interval + WRITE_STATS_INTERVAL > now) {
    goto done; /* Not ready to write */
  }

  /* Generate history string. */
  str = rep_hist_format_hs_stats(now);

  /* Reset HS history. */
  rep_hist_reset_hs_stats(now);

  /* Try to write to disk. */
  if (!check_or_create_data_subdir("stats")) {
    write_to_data_subdir("stats", "hidserv-stats", str,
                         "hidden service stats");
  }

done:
  tor_free(str);
  return start_of_hs_stats_interval + WRITE_STATS_INTERVAL;
}

static uint64_t link_proto_count[MAX_LINK_PROTO + 1][2];

/** Note that we negotiated link protocol version <b>link_proto</b>, on
 * a connection that started here iff <b>started_here</b> is true.
 */
void
rep_hist_note_negotiated_link_proto(unsigned link_proto, int started_here)
{
  started_here = !!started_here; /* force to 0 or 1 */
  if (link_proto > MAX_LINK_PROTO) {
    log_warn(LD_BUG, "Can't log link protocol %u", link_proto);
    return;
  }

  link_proto_count[link_proto][started_here]++;
}

/**
 * Update the maximum count of total pending channel padding timers
 * in this period.
 */
void
rep_hist_padding_count_timers(uint64_t num_timers)
{
  if (num_timers > padding_current.maximum_chanpad_timers) {
    padding_current.maximum_chanpad_timers = num_timers;
  }
}

/**
 * Count a cell that we sent for padding overhead statistics.
 *
 * RELAY_COMMAND_DROP and CELL_PADDING are accounted separately. Both should be
 * counted for PADDING_TYPE_TOTAL.
 */
void
rep_hist_padding_count_write(padding_type_t type)
{
  switch (type) {
  case PADDING_TYPE_DROP:
    padding_current.write_drop_cell_count++;
    break;
  case PADDING_TYPE_CELL:
    padding_current.write_pad_cell_count++;
    break;
  case PADDING_TYPE_TOTAL:
    padding_current.write_cell_count++;
    break;
  case PADDING_TYPE_ENABLED_TOTAL:
    padding_current.enabled_write_cell_count++;
    break;
  case PADDING_TYPE_ENABLED_CELL:
    padding_current.enabled_write_pad_cell_count++;
    break;
  }
}

/**
 * Count a cell that we've received for padding overhead statistics.
 *
 * RELAY_COMMAND_DROP and CELL_PADDING are accounted separately. Both should be
 * counted for PADDING_TYPE_TOTAL.
 */
void
rep_hist_padding_count_read(padding_type_t type)
{
  switch (type) {
  case PADDING_TYPE_DROP:
    padding_current.read_drop_cell_count++;
    break;
  case PADDING_TYPE_CELL:
    padding_current.read_pad_cell_count++;
    break;
  case PADDING_TYPE_TOTAL:
    padding_current.read_cell_count++;
    break;
  case PADDING_TYPE_ENABLED_TOTAL:
    padding_current.enabled_read_cell_count++;
    break;
  case PADDING_TYPE_ENABLED_CELL:
    padding_current.enabled_read_pad_cell_count++;
    break;
  }
}

/**
 * Reset our current padding statistics. Called once every 24 hours.
 */
void
rep_hist_reset_padding_counts(void)
{
  memset(&padding_current, 0, sizeof(padding_current));
}

/**
 * Copy our current cell counts into a structure for listing in our
 * extra-info descriptor. Also perform appropriate rounding and redaction.
 *
 * This function is called once every 24 hours.
 */
#define MIN_CELL_COUNTS_TO_PUBLISH 1
#define ROUND_CELL_COUNTS_TO 10000
void
rep_hist_prep_published_padding_counts(time_t now)
{
  memcpy(&padding_published, &padding_current, sizeof(padding_published));

  if (padding_published.read_cell_count < MIN_CELL_COUNTS_TO_PUBLISH ||
      padding_published.write_cell_count < MIN_CELL_COUNTS_TO_PUBLISH) {
    memset(&padding_published, 0, sizeof(padding_published));
    return;
  }

  format_iso_time(padding_published.first_published_at, now);
#define ROUND_AND_SET_COUNT(x) \
  (x) = round_uint64_to_next_multiple_of((x), ROUND_CELL_COUNTS_TO)
  ROUND_AND_SET_COUNT(padding_published.read_pad_cell_count);
  ROUND_AND_SET_COUNT(padding_published.write_pad_cell_count);
  ROUND_AND_SET_COUNT(padding_published.read_drop_cell_count);
  ROUND_AND_SET_COUNT(padding_published.write_drop_cell_count);
  ROUND_AND_SET_COUNT(padding_published.write_cell_count);
  ROUND_AND_SET_COUNT(padding_published.read_cell_count);
  ROUND_AND_SET_COUNT(padding_published.enabled_read_cell_count);
  ROUND_AND_SET_COUNT(padding_published.enabled_read_pad_cell_count);
  ROUND_AND_SET_COUNT(padding_published.enabled_write_cell_count);
  ROUND_AND_SET_COUNT(padding_published.enabled_write_pad_cell_count);
#undef ROUND_AND_SET_COUNT
}

/**
 * Returns an allocated string for extra-info documents for publishing
 * padding statistics from the last 24 hour interval.
 */
char *
rep_hist_get_padding_count_lines(void)
{
  char *result = NULL;

  if (!padding_published.read_cell_count ||
      !padding_published.write_cell_count) {
    return NULL;
  }

  tor_asprintf(
      &result,
      "padding-counts %s (%d s)"
      " bin-size=%" PRIu64 " write-drop=%" PRIu64 " write-pad=%" PRIu64
      " write-total=%" PRIu64 " read-drop=%" PRIu64 " read-pad=%" PRIu64
      " read-total=%" PRIu64 " enabled-read-pad=%" PRIu64
      " enabled-read-total=%" PRIu64 " enabled-write-pad=%" PRIu64
      " enabled-write-total=%" PRIu64 " max-chanpad-timers=%" PRIu64 "\n",
      padding_published.first_published_at,
      REPHIST_CELL_PADDING_COUNTS_INTERVAL, (uint64_t)ROUND_CELL_COUNTS_TO,
      (padding_published.write_drop_cell_count),
      (padding_published.write_pad_cell_count),
      (padding_published.write_cell_count),
      (padding_published.read_drop_cell_count),
      (padding_published.read_pad_cell_count),
      (padding_published.read_cell_count),
      (padding_published.enabled_read_pad_cell_count),
      (padding_published.enabled_read_cell_count),
      (padding_published.enabled_write_pad_cell_count),
      (padding_published.enabled_write_cell_count),
      (padding_published.maximum_chanpad_timers));

  return result;
}

/** Log a heartbeat message explaining how many connections of each link
 * protocol version we have used.
 */
void
rep_hist_log_link_protocol_counts(void)
{
  smartlist_t *lines = smartlist_new();

  for (int i = 1; i <= MAX_LINK_PROTO; i++) {
    char *line = NULL;
    tor_asprintf(&line,
                 "initiated %" PRIu64 " and received "
                 "%" PRIu64 " v%d connections",
                 link_proto_count[i][1], link_proto_count[i][0], i);
    smartlist_add(lines, line);
  }

  char *log_line = smartlist_join_strings(lines, "; ", 0, NULL);

  log_notice(LD_HEARTBEAT, "Since startup we %s.", log_line);

  SMARTLIST_FOREACH(lines, char *, s, tor_free(s));
  smartlist_free(lines);
  tor_free(log_line);
}

/** Free all storage held by the OR/link history caches, by the
 * bandwidth history arrays, by the port history, or by statistics . */
void
rep_hist_free_all(void)
{
  hs_stats_free(hs_stats);
  digestmap_free(history_map, free_or_history);

  bw_array_free(read_array);
  read_array = NULL;

  bw_array_free(write_array);
  write_array = NULL;

  bw_array_free(dir_read_array);
  dir_read_array = NULL;

  bw_array_free(dir_write_array);
  dir_write_array = NULL;

  tor_free(exit_bytes_read);
  tor_free(exit_bytes_written);
  tor_free(exit_streams);
  predicted_ports_free_all();
  bidi_map_free_all();

  if (circuits_for_buffer_stats) {
    SMARTLIST_FOREACH(circuits_for_buffer_stats, circ_buffer_stats_t *, s,
                      tor_free(s));
    smartlist_free(circuits_for_buffer_stats);
    circuits_for_buffer_stats = NULL;
  }
  rep_hist_desc_stats_term();
  total_descriptor_downloads = 0;

  tor_assert_nonfatal(rephist_total_alloc == 0);
  tor_assert_nonfatal_once(rephist_total_num == 0);
}
