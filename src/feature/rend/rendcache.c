/* Copyright (c) 2015-2019, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file rendcache.c
 * \brief Hidden service descriptor cache.
 **/

#define RENDCACHE_PRIVATE
#include "feature/rend/rendcache.h"

#include "app/config/config.h"
#include "feature/stats/rephist.h"
#include "feature/nodelist/routerlist.h"
#include "feature/rend/rendcommon.h"
#include "feature/rend/rendparse.h"

#include "core/or/extend_info_st.h"
#include "feature/rend/rend_intro_point_st.h"
#include "feature/rend/rend_service_descriptor_st.h"

#include "lib/ctime/di_ops.h"

/** Map from service id (as generated by rend_get_service_id) to
 * rend_cache_entry_t. */
STATIC strmap_t *rend_cache = NULL;

/** Map from service id to rend_cache_entry_t; only for hidden services. */
static strmap_t *rend_cache_local_service = NULL;

/** Map from descriptor id to rend_cache_entry_t; only for hidden service
 * directories. */
STATIC digestmap_t *rend_cache_v2_dir = NULL;

/** (Client side only) Map from service id to rend_cache_failure_t. This
 * cache is used to track intro point(IP) failures so we know when to keep
 * or discard a new descriptor we just fetched. Here is a description of the
 * cache behavior.
 *
 * Everytime tor discards an IP (ex: receives a NACK), we add an entry to
 * this cache noting the identity digest of the IP and it's failure type for
 * the service ID. The reason we indexed this cache by service ID is to
 * differentiate errors that can occur only for a specific service like a
 * NACK for instance. It applies for one but maybe not for the others.
 *
 * Once a service descriptor is fetched and considered valid, each IP is
 * looked up in this cache and if present, it is discarded from the fetched
 * descriptor. At the end, all IP(s) in the cache, for a specific service
 * ID, that were NOT present in the descriptor are removed from this cache.
 * Which means that if at least one IP was not in this cache, thus usable,
 * it's considered a new descriptor so we keep it. Else, if all IPs were in
 * this cache, we discard the descriptor as it's considered unusable.
 *
 * Once a descriptor is removed from the rend cache or expires, the entry
 * in this cache is also removed for the service ID.
 *
 * This scheme allows us to not rely on the descriptor's timestamp (which
 * is rounded down to the hour) to know if we have a newer descriptor. We
 * only rely on the usability of intro points from an internal state. */
STATIC strmap_t *rend_cache_failure = NULL;

/* DOCDOC */
STATIC size_t rend_cache_total_allocation = 0;

/** Initializes the service descriptor cache.
*/
void
rend_cache_init(void)
{
    rend_cache = strmap_new();
    rend_cache_v2_dir = digestmap_new();
    rend_cache_local_service = strmap_new();
    rend_cache_failure = strmap_new();
}

/** Return the approximate number of bytes needed to hold <b>e</b>. */
STATIC size_t
rend_cache_entry_allocation(const rend_cache_entry_t *e)
{
    if (!e) {
        return 0;
    }

    /* This doesn't count intro_nodes or key size */
    return sizeof(*e) + e->len + sizeof(*e->parsed);
}

/* DOCDOC */
size_t
rend_cache_get_total_allocation(void)
{
    return rend_cache_total_allocation;
}

/** Decrement the total bytes attributed to the rendezvous cache by n. */
void
rend_cache_decrement_allocation(size_t n)
{
    static int have_underflowed = 0;

    if (rend_cache_total_allocation >= n) {
        rend_cache_total_allocation -= n;
    } else {
        rend_cache_total_allocation = 0;
        if (! have_underflowed) {
            have_underflowed = 1;
            log_warn(LD_BUG, "Underflow in rend_cache_decrement_allocation");
        }
    }
}

/** Increase the total bytes attributed to the rendezvous cache by n. */
void
rend_cache_increment_allocation(size_t n)
{
    static int have_overflowed = 0;
    if (rend_cache_total_allocation <= SIZE_MAX - n) {
        rend_cache_total_allocation += n;
    } else {
        rend_cache_total_allocation = SIZE_MAX;
        if (! have_overflowed) {
            have_overflowed = 1;
            log_warn(LD_BUG, "Overflow in rend_cache_increment_allocation");
        }
    }
}

/** Helper: free a rend cache failure intro object. */
STATIC void
rend_cache_failure_intro_entry_free_(rend_cache_failure_intro_t *entry)
{
    if (entry == NULL) {
        return;
    }
    tor_free(entry);
}

static void
rend_cache_failure_intro_entry_free_void(void *entry)
{
    rend_cache_failure_intro_entry_free_(entry);
}

/** Allocate a rend cache failure intro object and return it. <b>failure</b>
 * is set into the object. This function can not fail. */
STATIC rend_cache_failure_intro_t *
rend_cache_failure_intro_entry_new(rend_intro_point_failure_t failure)
{
    rend_cache_failure_intro_t *entry = tor_malloc(sizeof(*entry));
    entry->failure_type = failure;
    entry->created_ts = time(NULL);
    return entry;
}

/** Helper: free a rend cache failure object. */
STATIC void
rend_cache_failure_entry_free_(rend_cache_failure_t *entry)
{
    if (entry == NULL) {
        return;
    }

    /* Free and remove every intro failure object. */
    digestmap_free(entry->intro_failures,
                   rend_cache_failure_intro_entry_free_void);

    tor_free(entry);
}

/** Helper: deallocate a rend_cache_failure_t. (Used with strmap_free(),
 * which requires a function pointer whose argument is void*). */
STATIC void
rend_cache_failure_entry_free_void(void *entry)
{
    rend_cache_failure_entry_free_(entry);
}

/** Allocate a rend cache failure object and return it. This function can
 * not fail. */
STATIC rend_cache_failure_t *
rend_cache_failure_entry_new(void)
{
    rend_cache_failure_t *entry = tor_malloc(sizeof(*entry));
    entry->intro_failures = digestmap_new();
    return entry;
}

/** Remove failure cache entry for the service ID in the given descriptor
 * <b>desc</b>. */
STATIC void
rend_cache_failure_remove(rend_service_descriptor_t *desc)
{
    char service_id[REND_SERVICE_ID_LEN_BASE32 + 1];
    rend_cache_failure_t *entry;

    if (desc == NULL) {
        return;
    }
    if (rend_get_service_id(desc->pk, service_id) < 0) {
        return;
    }
    entry = strmap_get_lc(rend_cache_failure, service_id);
    if (entry != NULL) {
        strmap_remove_lc(rend_cache_failure, service_id);
        rend_cache_failure_entry_free(entry);
    }
}

/** Helper: free storage held by a single service descriptor cache entry. */
STATIC void
rend_cache_entry_free_(rend_cache_entry_t *e)
{
    if (!e) {
        return;
    }
    rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
    /* We are about to remove a descriptor from the cache so remove the entry
     * in the failure cache. */
    rend_cache_failure_remove(e->parsed);
    rend_service_descriptor_free(e->parsed);
    tor_free(e->desc);
    tor_free(e);
}

/** Helper: deallocate a rend_cache_entry_t.  (Used with strmap_free(), which
 * requires a function pointer whose argument is void*). */
static void
rend_cache_entry_free_void(void *p)
{
    rend_cache_entry_free_(p);
}

/** Check if a failure cache entry exists for the given intro point. */
bool
rend_cache_intro_failure_exists(const char *service_id,
                                const uint8_t *intro_identity)
{
    tor_assert(service_id);
    tor_assert(intro_identity);

    return cache_failure_intro_lookup(intro_identity, service_id, NULL);
}

/** Free all storage held by the service descriptor cache. */
void
rend_cache_free_all(void)
{
    strmap_free(rend_cache, rend_cache_entry_free_void);
    digestmap_free(rend_cache_v2_dir, rend_cache_entry_free_void);
    strmap_free(rend_cache_local_service, rend_cache_entry_free_void);
    strmap_free(rend_cache_failure, rend_cache_failure_entry_free_void);
    rend_cache = NULL;
    rend_cache_v2_dir = NULL;
    rend_cache_local_service = NULL;
    rend_cache_failure = NULL;
    rend_cache_total_allocation = 0;
}

/** Remove all entries that re REND_CACHE_FAILURE_MAX_AGE old. This is
 * called every second.
 *
 * We have to clean these regurlarly else if for whatever reasons an hidden
 * service goes offline and a client tries to connect to it during that
 * time, a failure entry is created and the client will be unable to connect
 * for a while even though the service has return online.  */
void
rend_cache_failure_clean(time_t now)
{
    time_t cutoff = now - REND_CACHE_FAILURE_MAX_AGE;
    STRMAP_FOREACH_MODIFY(rend_cache_failure, key,
                          rend_cache_failure_t *, ent) {
        /* Free and remove every intro failure object that match the cutoff. */
        DIGESTMAP_FOREACH_MODIFY(ent->intro_failures, ip_key,
                                 rend_cache_failure_intro_t *, ip_ent) {
            if (ip_ent->created_ts < cutoff) {
                rend_cache_failure_intro_entry_free(ip_ent);
                MAP_DEL_CURRENT(ip_key);
            }
        }
        DIGESTMAP_FOREACH_END;
        /* If the entry is now empty of intro point failures, remove it. */
        if (digestmap_isempty(ent->intro_failures)) {
            rend_cache_failure_entry_free(ent);
            MAP_DEL_CURRENT(key);
        }
    }
    STRMAP_FOREACH_END;
}

/** Removes all old entries from the client or service descriptor cache.
*/
void
rend_cache_clean(time_t now, rend_cache_type_t cache_type)
{
    strmap_iter_t *iter;
    const char *key;
    void *val;
    rend_cache_entry_t *ent;
    time_t cutoff = now - REND_CACHE_MAX_AGE - REND_CACHE_MAX_SKEW;
    strmap_t *cache = NULL;

    if (cache_type == REND_CACHE_TYPE_CLIENT) {
        cache = rend_cache;
    } else if (cache_type == REND_CACHE_TYPE_SERVICE)  {
        cache = rend_cache_local_service;
    }
    tor_assert(cache);

    for (iter = strmap_iter_init(cache); !strmap_iter_done(iter); ) {
        strmap_iter_get(iter, &key, &val);
        ent = (rend_cache_entry_t*)val;
        if (ent->parsed->timestamp < cutoff) {
            iter = strmap_iter_next_rmv(cache, iter);
            rend_cache_entry_free(ent);
        } else {
            iter = strmap_iter_next(cache, iter);
        }
    }
}

/** Remove ALL entries from the rendezvous service descriptor cache.
*/
void
rend_cache_purge(void)
{
    if (rend_cache) {
        log_info(LD_REND, "Purging HS v2 descriptor cache");
        strmap_free(rend_cache, rend_cache_entry_free_void);
    }
    rend_cache = strmap_new();
}

/** Remove ALL entries from the failure cache. This is also called when a
 * NEWNYM signal is received. */
void
rend_cache_failure_purge(void)
{
    if (rend_cache_failure) {
        log_info(LD_REND, "Purging HS v2 failure cache");
        strmap_free(rend_cache_failure, rend_cache_failure_entry_free_void);
    }
    rend_cache_failure = strmap_new();
}

/** Lookup the rend failure cache using a relay identity digest in
 * <b>identity</b> which has DIGEST_LEN bytes and service ID <b>service_id</b>
 * which is a null-terminated string. If found, the intro failure is set in
 * <b>intro_entry</b> else it stays untouched. Return 1 iff found else 0. */
STATIC int
cache_failure_intro_lookup(const uint8_t *identity, const char *service_id,
                           rend_cache_failure_intro_t **intro_entry)
{
    rend_cache_failure_t *elem;
    rend_cache_failure_intro_t *intro_elem;

    tor_assert(rend_cache_failure);

    if (intro_entry) {
        *intro_entry = NULL;
    }

    /* Lookup descriptor and return it. */
    elem = strmap_get_lc(rend_cache_failure, service_id);
    if (elem == NULL) {
        goto not_found;
    }
    intro_elem = digestmap_get(elem->intro_failures, (char *) identity);
    if (intro_elem == NULL) {
        goto not_found;
    }
    if (intro_entry) {
        *intro_entry = intro_elem;
    }
    return 1;
not_found:
    return 0;
}

/** Allocate a new cache failure intro object and copy the content from
 * <b>entry</b> to this newly allocated object. Return it. */
static rend_cache_failure_intro_t *
cache_failure_intro_dup(const rend_cache_failure_intro_t *entry)
{
    rend_cache_failure_intro_t *ent_dup =
        rend_cache_failure_intro_entry_new(entry->failure_type);
    ent_dup->created_ts = entry->created_ts;
    return ent_dup;
}

/** Add an intro point failure to the failure cache using the relay
 * <b>identity</b> and service ID <b>service_id</b>. Record the
 * <b>failure</b> in that object. */
STATIC void
cache_failure_intro_add(const uint8_t *identity, const char *service_id,
                        rend_intro_point_failure_t failure)
{
    rend_cache_failure_t *fail_entry;
    rend_cache_failure_intro_t *entry, *old_entry;

    /* Make sure we have a failure object for this service ID and if not,
     * create it with this new intro failure entry. */
    fail_entry = strmap_get_lc(rend_cache_failure, service_id);
    if (fail_entry == NULL) {
        fail_entry = rend_cache_failure_entry_new();
        /* Add failure entry to global rend failure cache. */
        strmap_set_lc(rend_cache_failure, service_id, fail_entry);
    }
    entry = rend_cache_failure_intro_entry_new(failure);
    old_entry = digestmap_set(fail_entry->intro_failures,
                              (char *) identity, entry);
    /* This _should_ be NULL, but in case it isn't, free it. */
    rend_cache_failure_intro_entry_free(old_entry);
}

/** Using a parsed descriptor <b>desc</b>, check if the introduction points
 * are present in the failure cache and if so they are removed from the
 * descriptor and kept into the failure cache. Then, each intro points that
 * are NOT in the descriptor but in the failure cache for the given
 * <b>service_id</b> are removed from the failure cache. */
STATIC void
validate_intro_point_failure(const rend_service_descriptor_t *desc,
                             const char *service_id)
{
    rend_cache_failure_t *new_entry, *cur_entry;
    /* New entry for the service ID that will be replacing the one in the
     * failure cache since we have a new descriptor. In the case where all
     * intro points are removed, we are assured that the new entry is the same
     * as the current one. */
    new_entry = tor_malloc(sizeof(*new_entry));
    new_entry->intro_failures = digestmap_new();

    tor_assert(desc);

    SMARTLIST_FOREACH_BEGIN(desc->intro_nodes, rend_intro_point_t *, intro) {
        int found;
        rend_cache_failure_intro_t *entry;
        const uint8_t *identity =
            (uint8_t *) intro->extend_info->identity_digest;

        found = cache_failure_intro_lookup(identity, service_id, &entry);
        if (found) {
            /* Dup here since it will be freed at the end when removing the
             * original entry in the cache. */
            rend_cache_failure_intro_t *ent_dup = cache_failure_intro_dup(entry);
            /* This intro point is in our cache, discard it from the descriptor
             * because chances are that it's unusable. */
            SMARTLIST_DEL_CURRENT(desc->intro_nodes, intro);
            /* Keep it for our new entry. */
            digestmap_set(new_entry->intro_failures, (char *) identity, ent_dup);
            /* Only free it when we're done looking at it. */
            rend_intro_point_free(intro);
            continue;
        }
    }
    SMARTLIST_FOREACH_END(intro);

    /* Swap the failure entry in the cache and free the current one. */
    cur_entry = strmap_get_lc(rend_cache_failure, service_id);
    if (cur_entry != NULL) {
        rend_cache_failure_entry_free(cur_entry);
    }
    strmap_set_lc(rend_cache_failure, service_id, new_entry);
}

/** Note down an intro failure in the rend failure cache using the type of
 * failure in <b>failure</b> for the relay identity digest in
 * <b>identity</b> and service ID <b>service_id</b>. If an entry already
 * exists in the cache, the failure type is changed with <b>failure</b>. */
void
rend_cache_intro_failure_note(rend_intro_point_failure_t failure,
                              const uint8_t *identity,
                              const char *service_id)
{
    int found;
    rend_cache_failure_intro_t *entry;

    found = cache_failure_intro_lookup(identity, service_id, &entry);
    if (!found) {
        cache_failure_intro_add(identity, service_id, failure);
    } else {
        /* Replace introduction point failure with this one. */
        entry->failure_type = failure;
    }
}

/** Remove all old v2 descriptors and those for which this hidden service
 * directory is not responsible for any more. The cutoff is the time limit for
 * which we want to keep the cache entry. In other words, any entry created
 * before will be removed. */
size_t
rend_cache_clean_v2_descs_as_dir(time_t cutoff)
{
    digestmap_iter_t *iter;
    size_t bytes_removed = 0;

    for (iter = digestmap_iter_init(rend_cache_v2_dir);
         !digestmap_iter_done(iter); ) {
        const char *key;
        void *val;
        rend_cache_entry_t *ent;
        digestmap_iter_get(iter, &key, &val);
        ent = val;
        if (ent->parsed->timestamp < cutoff) {
            char key_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
            base32_encode(key_base32, sizeof(key_base32), key, DIGEST_LEN);
            log_info(LD_REND, "Removing descriptor with ID '%s' from cache",
                     safe_str_client(key_base32));
            bytes_removed += rend_cache_entry_allocation(ent);
            iter = digestmap_iter_next_rmv(rend_cache_v2_dir, iter);
            rend_cache_entry_free(ent);
        } else {
            iter = digestmap_iter_next(rend_cache_v2_dir, iter);
        }
    }

    return bytes_removed;
}

/** Lookup in the client cache the given service ID <b>query</b> for
 * <b>version</b>.
 *
 * Return 0 if found and if <b>e</b> is non NULL, set it with the entry
 * found. Else, a negative value is returned and <b>e</b> is untouched.
 * -EINVAL means that <b>query</b> is not a valid service id.
 * -ENOENT means that no entry in the cache was found. */
int
rend_cache_lookup_entry(const char *query, int version, rend_cache_entry_t **e)
{
    int ret = 0;
    char key[REND_SERVICE_ID_LEN_BASE32 + 2]; /* <version><query>\0 */
    rend_cache_entry_t *entry = NULL;
    static const int default_version = 2;

    tor_assert(rend_cache);
    tor_assert(query);

    if (!rend_valid_v2_service_id(query)) {
        ret = -EINVAL;
        goto end;
    }

    switch (version) {
    case 0:
        log_warn(LD_REND, "Cache lookup of a v0 renddesc is deprecated.");
        break;
    case 2:
    /* Default is version 2. */
    default:
        tor_snprintf(key, sizeof(key), "%d%s", default_version, query);
        entry = strmap_get_lc(rend_cache, key);
        break;
    }
    if (!entry) {
        ret = -ENOENT;
        goto end;
    }
    tor_assert(entry->parsed && entry->parsed->intro_nodes);

    if (e) {
        *e = entry;
    }

end:
    return ret;
}

/*
 * Lookup the v2 service descriptor with the service ID <b>query</b> in the
 * local service descriptor cache. Return 0 if found and if <b>e</b> is
 * non NULL, set it with the entry found. Else, a negative value is returned
 * and <b>e</b> is untouched.
 * -EINVAL means that <b>query</b> is not a valid service id.
 * -ENOENT means that no entry in the cache was found. */
int
rend_cache_lookup_v2_desc_as_service(const char *query, rend_cache_entry_t **e)
{
    int ret = 0;
    rend_cache_entry_t *entry = NULL;

    tor_assert(rend_cache_local_service);
    tor_assert(query);

    if (!rend_valid_v2_service_id(query)) {
        ret = -EINVAL;
        goto end;
    }

    /* Lookup descriptor and return. */
    entry = strmap_get_lc(rend_cache_local_service, query);
    if (!entry) {
        ret = -ENOENT;
        goto end;
    }

    if (e) {
        *e = entry;
    }

end:
    return ret;
}

/** Lookup the v2 service descriptor with base32-encoded <b>desc_id</b> and
 * copy the pointer to it to *<b>desc</b>.  Return 1 on success, 0 on
 * well-formed-but-not-found, and -1 on failure.
 */
int
rend_cache_lookup_v2_desc_as_dir(const char *desc_id, const char **desc)
{
    rend_cache_entry_t *e;
    char desc_id_digest[DIGEST_LEN];
    tor_assert(rend_cache_v2_dir);
    if (base32_decode(desc_id_digest, DIGEST_LEN,
                      desc_id, REND_DESC_ID_V2_LEN_BASE32) != DIGEST_LEN) {
        log_fn(LOG_PROTOCOL_WARN, LD_REND,
               "Rejecting v2 rendezvous descriptor request -- descriptor ID "
               "has wrong length or illegal characters: %s",
               safe_str(desc_id));
        return -1;
    }
    /* Lookup descriptor and return. */
    e = digestmap_get(rend_cache_v2_dir, desc_id_digest);
    if (e) {
        *desc = e->desc;
        e->last_served = approx_time();
        return 1;
    }
    return 0;
}

/** Parse the v2 service descriptor(s) in <b>desc</b> and store it/them to the
 * local rend cache. Don't attempt to decrypt the included list of introduction
 * points (as we don't have a descriptor cookie for it).
 *
 * If we have a newer descriptor with the same ID, ignore this one.
 * If we have an older descriptor with the same ID, replace it.
 *
 * Return 0 on success, or -1 if we couldn't parse any of them.
 *
 * We should only call this function for public (e.g. non bridge) relays.
 */
int
rend_cache_store_v2_desc_as_dir(const char *desc)
{
    const or_options_t *options = get_options();
    rend_service_descriptor_t *parsed;
    char desc_id[DIGEST_LEN];
    char *intro_content;
    size_t intro_size;
    size_t encoded_size;
    char desc_id_base32[REND_DESC_ID_V2_LEN_BASE32 + 1];
    int number_parsed = 0, number_stored = 0;
    const char *current_desc = desc;
    const char *next_desc;
    rend_cache_entry_t *e;
    time_t now = time(NULL);
    tor_assert(rend_cache_v2_dir);
    tor_assert(desc);
    while (rend_parse_v2_service_descriptor(&parsed, desc_id, &intro_content,
                                            &intro_size, &encoded_size,
                                            &next_desc, current_desc, 1) >= 0) {
        number_parsed++;
        /* We don't care about the introduction points. */
        tor_free(intro_content);
        /* For pretty log statements. */
        base32_encode(desc_id_base32, sizeof(desc_id_base32),
                      desc_id, DIGEST_LEN);
        /* Is descriptor too old? */
        if (parsed->timestamp < now - REND_CACHE_MAX_AGE-REND_CACHE_MAX_SKEW) {
            log_info(LD_REND, "Service descriptor with desc ID %s is too old.",
                     safe_str(desc_id_base32));
            goto skip;
        }
        /* Is descriptor too far in the future? */
        if (parsed->timestamp > now + REND_CACHE_MAX_SKEW) {
            log_info(LD_REND, "Service descriptor with desc ID %s is too far in the "
                     "future.",
                     safe_str(desc_id_base32));
            goto skip;
        }
        /* Do we already have a newer descriptor? */
        e = digestmap_get(rend_cache_v2_dir, desc_id);
        if (e && e->parsed->timestamp > parsed->timestamp) {
            log_info(LD_REND, "We already have a newer service descriptor with the "
                     "same desc ID %s and version.",
                     safe_str(desc_id_base32));
            goto skip;
        }
        /* Do we already have this descriptor? */
        if (e && !strcmp(desc, e->desc)) {
            log_info(LD_REND, "We already have this service descriptor with desc "
                     "ID %s.", safe_str(desc_id_base32));
            goto skip;
        }
        /* Store received descriptor. */
        if (!e) {
            e = tor_malloc_zero(sizeof(rend_cache_entry_t));
            digestmap_set(rend_cache_v2_dir, desc_id, e);
            /* Treat something just uploaded as having been served a little
             * while ago, so that flooding with new descriptors doesn't help
             * too much.
             */
            e->last_served = approx_time() - 3600;
        } else {
            rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
            rend_service_descriptor_free(e->parsed);
            tor_free(e->desc);
        }
        e->parsed = parsed;
        e->desc = tor_strndup(current_desc, encoded_size);
        e->len = encoded_size;
        rend_cache_increment_allocation(rend_cache_entry_allocation(e));
        log_info(LD_REND, "Successfully stored service descriptor with desc ID "
                 "'%s' and len %d.",
                 safe_str(desc_id_base32), (int)encoded_size);
        /* Statistics: Note down this potentially new HS. */
        if (options->HiddenServiceStatistics) {
            rep_hist_stored_maybe_new_hs(e->parsed->pk);
        }

        number_stored++;
        goto advance;
skip:
        rend_service_descriptor_free(parsed);
advance:
        /* advance to next descriptor, if available. */
        current_desc = next_desc;
        /* check if there is a next descriptor. */
        if (!current_desc ||
            strcmpstart(current_desc, "rendezvous-service-descriptor ")) {
            break;
        }
    }
    if (!number_parsed) {
        log_info(LD_REND, "Could not parse any descriptor.");
        return -1;
    }
    log_info(LD_REND, "Parsed %d and added %d descriptor%s.",
             number_parsed, number_stored, number_stored != 1 ? "s" : "");
    return 0;
}

/** Parse the v2 service descriptor in <b>desc</b> and store it to the
* local service rend cache. Don't attempt to decrypt the included list of
* introduction points.
*
* If we have a newer descriptor with the same ID, ignore this one.
* If we have an older descriptor with the same ID, replace it.
*
* Return 0 on success, or -1 if we couldn't understand the descriptor.
*/
int
rend_cache_store_v2_desc_as_service(const char *desc)
{
    rend_service_descriptor_t *parsed = NULL;
    char desc_id[DIGEST_LEN];
    char *intro_content = NULL;
    size_t intro_size;
    size_t encoded_size;
    const char *next_desc;
    char service_id[REND_SERVICE_ID_LEN_BASE32+1];
    rend_cache_entry_t *e;
    int retval = -1;
    tor_assert(rend_cache_local_service);
    tor_assert(desc);

    /* Parse the descriptor. */
    if (rend_parse_v2_service_descriptor(&parsed, desc_id, &intro_content,
                                         &intro_size, &encoded_size,
                                         &next_desc, desc, 0) < 0) {
        log_warn(LD_REND, "Could not parse descriptor.");
        goto err;
    }
    /* Compute service ID from public key. */
    if (rend_get_service_id(parsed->pk, service_id)<0) {
        log_warn(LD_REND, "Couldn't compute service ID.");
        goto err;
    }

    /* Do we already have a newer descriptor? Allow new descriptors with a
       rounded timestamp equal to or newer than the current descriptor */
    e = (rend_cache_entry_t*) strmap_get_lc(rend_cache_local_service,
                                            service_id);
    if (e && e->parsed->timestamp > parsed->timestamp) {
        log_info(LD_REND, "We already have a newer service descriptor for "
                 "service ID %s.", safe_str_client(service_id));
        goto okay;
    }
    /* We don't care about the introduction points. */
    tor_free(intro_content);
    if (!e) {
        e = tor_malloc_zero(sizeof(rend_cache_entry_t));
        strmap_set_lc(rend_cache_local_service, service_id, e);
    } else {
        rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
        rend_service_descriptor_free(e->parsed);
        tor_free(e->desc);
    }
    e->parsed = parsed;
    e->desc = tor_malloc_zero(encoded_size + 1);
    strlcpy(e->desc, desc, encoded_size + 1);
    e->len = encoded_size;
    rend_cache_increment_allocation(rend_cache_entry_allocation(e));
    log_debug(LD_REND,"Successfully stored rend desc '%s', len %d.",
              safe_str_client(service_id), (int)encoded_size);
    return 0;

okay:
    retval = 0;

err:
    rend_service_descriptor_free(parsed);
    tor_free(intro_content);
    return retval;
}

/** Parse the v2 service descriptor in <b>desc</b>, decrypt the included list
 * of introduction points with <b>descriptor_cookie</b> (which may also be
 * <b>NULL</b> if decryption is not necessary), and store the descriptor to
 * the local cache under its version and service id.
 *
 * If we have a newer v2 descriptor with the same ID, ignore this one.
 * If we have an older descriptor with the same ID, replace it.
 * If the descriptor's service ID does not match
 * <b>rend_query</b>-\>onion_address, reject it.
 *
 * If the descriptor's descriptor ID doesn't match <b>desc_id_base32</b>,
 * reject it.
 *
 * Return 0 on success, or -1 if we rejected the descriptor.
 * If entry is not NULL, set it with the cache entry pointer of the descriptor.
 */
int
rend_cache_store_v2_desc_as_client(const char *desc,
                                   const char *desc_id_base32,
                                   const rend_data_t *rend_query,
                                   rend_cache_entry_t **entry)
{
    /*XXXX this seems to have a bit of duplicate code with
     * rend_cache_store_v2_desc_as_dir().  Fix that. */
    /* Though having similar elements, both functions were separated on
     * purpose:
     * - dirs don't care about encoded/encrypted introduction points, clients
     *   do.
     * - dirs store descriptors in a separate cache by descriptor ID, whereas
     *   clients store them by service ID; both caches are different data
     *   structures and have different access methods.
     * - dirs store a descriptor only if they are responsible for its ID,
     *   clients do so in every way (because they have requested it before).
     * - dirs can process multiple concatenated descriptors which is required
     *   for replication, whereas clients only accept a single descriptor.
     * Thus, combining both methods would result in a lot of if statements
     * which probably would not improve, but worsen code readability. -KL */
    rend_service_descriptor_t *parsed = NULL;
    char desc_id[DIGEST_LEN];
    char *intro_content = NULL;
    size_t intro_size;
    size_t encoded_size;
    const char *next_desc;
    time_t now = time(NULL);
    char key[REND_SERVICE_ID_LEN_BASE32+2];
    char service_id[REND_SERVICE_ID_LEN_BASE32+1];
    char want_desc_id[DIGEST_LEN];
    rend_cache_entry_t *e;
    int retval = -1;
    rend_data_v2_t *rend_data = TO_REND_DATA_V2(rend_query);

    tor_assert(rend_cache);
    tor_assert(desc);
    tor_assert(desc_id_base32);
    memset(want_desc_id, 0, sizeof(want_desc_id));
    if (entry) {
        *entry = NULL;
    }
    if (base32_decode(want_desc_id, sizeof(want_desc_id),
                      desc_id_base32, strlen(desc_id_base32)) !=
        sizeof(want_desc_id)) {
        log_warn(LD_BUG, "Couldn't decode base32 %s for descriptor id.",
                 escaped_safe_str_client(desc_id_base32));
        goto err;
    }
    /* Parse the descriptor. */
    if (rend_parse_v2_service_descriptor(&parsed, desc_id, &intro_content,
                                         &intro_size, &encoded_size,
                                         &next_desc, desc, 0) < 0) {
        log_warn(LD_REND, "Could not parse descriptor.");
        goto err;
    }
    /* Compute service ID from public key. */
    if (rend_get_service_id(parsed->pk, service_id)<0) {
        log_warn(LD_REND, "Couldn't compute service ID.");
        goto err;
    }
    if (rend_data->onion_address[0] != '\0' &&
        strcmp(rend_data->onion_address, service_id)) {
        log_warn(LD_REND, "Received service descriptor for service ID %s; "
                 "expected descriptor for service ID %s.",
                 service_id, safe_str(rend_data->onion_address));
        goto err;
    }
    if (tor_memneq(desc_id, want_desc_id, DIGEST_LEN)) {
        log_warn(LD_REND, "Received service descriptor for %s with incorrect "
                 "descriptor ID.", service_id);
        goto err;
    }

    /* Decode/decrypt introduction points. */
    if (intro_content && intro_size > 0) {
        int n_intro_points;
        if (rend_data->auth_type != REND_NO_AUTH &&
            !safe_mem_is_zero(rend_data->descriptor_cookie,
                              sizeof(rend_data->descriptor_cookie))) {
            char *ipos_decrypted = NULL;
            size_t ipos_decrypted_size;
            if (rend_decrypt_introduction_points(&ipos_decrypted,
                                                 &ipos_decrypted_size,
                                                 rend_data->descriptor_cookie,
                                                 intro_content,
                                                 intro_size) < 0) {
                log_warn(LD_REND, "Failed to decrypt introduction points. We are "
                         "probably unable to parse the encoded introduction points.");
            } else {
                /* Replace encrypted with decrypted introduction points. */
                log_info(LD_REND, "Successfully decrypted introduction points.");
                tor_free(intro_content);
                intro_content = ipos_decrypted;
                intro_size = ipos_decrypted_size;
            }
        }
        n_intro_points = rend_parse_introduction_points(parsed, intro_content,
                         intro_size);
        if (n_intro_points <= 0) {
            log_warn(LD_REND, "Failed to parse introduction points. Either the "
                     "service has published a corrupt descriptor or you have "
                     "provided invalid authorization data.");
            goto err;
        } else if (n_intro_points > MAX_INTRO_POINTS) {
            log_warn(LD_REND, "Found too many introduction points on a hidden "
                     "service descriptor for %s. This is probably a (misguided) "
                     "attempt to improve reliability, but it could also be an "
                     "attempt to do a guard enumeration attack. Rejecting.",
                     safe_str_client(service_id));

            goto err;
        }
    } else {
        log_info(LD_REND, "Descriptor does not contain any introduction points.");
        parsed->intro_nodes = smartlist_new();
    }
    /* We don't need the encoded/encrypted introduction points any longer. */
    tor_free(intro_content);
    /* Is descriptor too old? */
    if (parsed->timestamp < now - REND_CACHE_MAX_AGE-REND_CACHE_MAX_SKEW) {
        log_warn(LD_REND, "Service descriptor with service ID %s is too old.",
                 safe_str_client(service_id));
        goto err;
    }
    /* Is descriptor too far in the future? */
    if (parsed->timestamp > now + REND_CACHE_MAX_SKEW) {
        log_warn(LD_REND, "Service descriptor with service ID %s is too far in "
                 "the future.", safe_str_client(service_id));
        goto err;
    }
    /* Do we have the same exact copy already in our cache? */
    tor_snprintf(key, sizeof(key), "2%s", service_id);
    e = (rend_cache_entry_t*) strmap_get_lc(rend_cache, key);
    if (e && !strcmp(desc, e->desc)) {
        log_info(LD_REND,"We already have this service descriptor %s.",
                 safe_str_client(service_id));
        goto okay;
    }
    /* Verify that we are not replacing an older descriptor. It's important to
     * avoid an evil HSDir serving old descriptor. We validate if the
     * timestamp is greater than and not equal because it's a rounded down
     * timestamp to the hour so if the descriptor changed in the same hour,
     * the rend cache failure will tell us if we have a new descriptor. */
    if (e && e->parsed->timestamp > parsed->timestamp) {
        log_info(LD_REND, "We already have a new enough service descriptor for "
                 "service ID %s with the same desc ID and version.",
                 safe_str_client(service_id));
        goto okay;
    }
    /* Lookup our failure cache for intro point that might be unusable. */
    validate_intro_point_failure(parsed, service_id);
    /* It's now possible that our intro point list is empty, which means that
     * this descriptor is useless to us because intro points have all failed
     * somehow before. Discard the descriptor. */
    if (smartlist_len(parsed->intro_nodes) == 0) {
        log_info(LD_REND, "Service descriptor with service ID %s has no "
                 "usable intro points. Discarding it.",
                 safe_str_client(service_id));
        goto err;
    }
    /* Now either purge the current one and replace its content or create a
     * new one and add it to the rend cache. */
    if (!e) {
        e = tor_malloc_zero(sizeof(rend_cache_entry_t));
        strmap_set_lc(rend_cache, key, e);
    } else {
        rend_cache_decrement_allocation(rend_cache_entry_allocation(e));
        rend_cache_failure_remove(e->parsed);
        rend_service_descriptor_free(e->parsed);
        tor_free(e->desc);
    }
    e->parsed = parsed;
    e->desc = tor_malloc_zero(encoded_size + 1);
    strlcpy(e->desc, desc, encoded_size + 1);
    e->len = encoded_size;
    rend_cache_increment_allocation(rend_cache_entry_allocation(e));
    log_debug(LD_REND,"Successfully stored rend desc '%s', len %d.",
              safe_str_client(service_id), (int)encoded_size);
    if (entry) {
        *entry = e;
    }
    return 0;

okay:
    if (entry) {
        *entry = e;
    }
    retval = 0;

err:
    rend_service_descriptor_free(parsed);
    tor_free(intro_content);
    return retval;
}
