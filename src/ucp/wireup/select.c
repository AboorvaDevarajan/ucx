/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2016.  ALL RIGHTS RESERVED.
 * Copyright (C) Los Alamos National Security, LLC. 2019 ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wireup.h"
#include "address.h"

#include <ucs/algorithm/qsort_r.h>
#include <ucs/datastruct/queue.h>
#include <ucs/sys/sock.h>
#include <ucp/core/ucp_ep.inl>
#include <string.h>
#include <inttypes.h>

#define UCP_WIREUP_RMA_BW_TEST_MSG_SIZE       262144

#define UCP_WIREUP_CHECK_AMO_FLAGS(_ae, _criteria, _context, _addr_index, _op, _size)      \
    if (!ucs_test_all_flags((_ae)->iface_attr.atomic.atomic##_size._op##_flags,            \
                            (_criteria)->remote_atomic_flags.atomic##_size._op##_flags)) { \
        char desc[256];                                                                    \
        ucs_trace("addr[%d] %s: no %s", (_addr_index),                                     \
                  ucp_find_tl_name_by_csum((_context), (_ae)->tl_name_csum),               \
                  ucp_wireup_get_missing_amo_flag_desc_##_op(                              \
                      (_ae)->iface_attr.atomic.atomic##_size._op##_flags,                  \
                      (_criteria)->remote_atomic_flags.atomic##_size._op##_flags,          \
                      (_size), desc, sizeof(desc)));                                       \
        continue;                                                                          \
    }

typedef struct ucp_wireup_atomic_flag {
    const char *name;
    const char *fetch;
} ucp_wireup_atomic_flag_t;


enum {
    UCP_WIREUP_LANE_USAGE_AM     = UCS_BIT(0), /* Active messages */
    UCP_WIREUP_LANE_USAGE_AM_BW  = UCS_BIT(1), /* High-BW active messages */
    UCP_WIREUP_LANE_USAGE_RMA    = UCS_BIT(2), /* Remote memory access */
    UCP_WIREUP_LANE_USAGE_RMA_BW = UCS_BIT(3), /* High-BW remote memory access */
    UCP_WIREUP_LANE_USAGE_AMO    = UCS_BIT(4), /* Atomic memory access */
    UCP_WIREUP_LANE_USAGE_TAG    = UCS_BIT(5)  /* Tag matching offload */
};


typedef struct {
    ucp_rsc_index_t   rsc_index;
    unsigned          addr_index;
    ucp_lane_index_t  proxy_lane;
    ucp_rsc_index_t   dst_md_index;
    uint32_t          usage;
    double            am_bw_score;
    double            rma_score;
    double            rma_bw_score;
    double            amo_score;
} ucp_wireup_lane_desc_t;


typedef struct {
    ucp_wireup_criteria_t criteria;
    uint64_t              local_dev_bitmap;
    uint64_t              remote_dev_bitmap;
    ucp_md_map_t          md_map;
    uint32_t              usage;
    unsigned              max_lanes;
} ucp_wireup_select_bw_info_t;


/**
 * Context for lanes selection during UCP wireup procedure
 */
typedef struct {
    ucp_ep_h                  ep;                        /* UCP Endpoint */
    const ucp_ep_params_t     *params;                   /* Tuning parameters for the
                                                          * UCP endpoint */
    unsigned                  ep_init_flags;             /* Endpoint init flags */
    const ucp_address_entry_t *address_list;             /* Array of remote addresses */
    unsigned                  address_count;             /* Number of remote addresses */
    ucp_wireup_lane_desc_t    lane_descs[UCP_MAX_LANES]; /* Array of active lanes that are
                                                          * found during selection */
    ucp_lane_index_t          num_lanes;                 /* Number of active lanes */
    int                       allow_am;                  /* Shows whether emulation over AM
                                                          * is allowed or not for RMA/AMO */
    ucp_wireup_select_info_t  am_info;                   /* AM transport selection info */
} ucp_wireup_select_ctx_t;


static const char *ucp_wireup_md_flags[] = {
    [ucs_ilog2(UCT_MD_FLAG_ALLOC)]               = "memory allocation",
    [ucs_ilog2(UCT_MD_FLAG_REG)]                 = "memory registration",
};

static const char *ucp_wireup_iface_flags[] = {
    [ucs_ilog2(UCT_IFACE_FLAG_AM_SHORT)]         = "am short",
    [ucs_ilog2(UCT_IFACE_FLAG_AM_BCOPY)]         = "am bcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_AM_ZCOPY)]         = "am zcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_PUT_SHORT)]        = "put short",
    [ucs_ilog2(UCT_IFACE_FLAG_PUT_BCOPY)]        = "put bcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_PUT_ZCOPY)]        = "put zcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_GET_SHORT)]        = "get short",
    [ucs_ilog2(UCT_IFACE_FLAG_GET_BCOPY)]        = "get bcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_GET_ZCOPY)]        = "get zcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_ERRHANDLE_PEER_FAILURE)] = "peer failure handler",
    [ucs_ilog2(UCT_IFACE_FLAG_CONNECT_TO_IFACE)] = "connect to iface",
    [ucs_ilog2(UCT_IFACE_FLAG_CONNECT_TO_EP)]    = "connect to ep",
    [ucs_ilog2(UCT_IFACE_FLAG_AM_DUP)]           = "full reliability",
    [ucs_ilog2(UCT_IFACE_FLAG_CB_SYNC)]          = "sync callback",
    [ucs_ilog2(UCT_IFACE_FLAG_CB_ASYNC)]         = "async callback",
    [ucs_ilog2(UCT_IFACE_FLAG_EVENT_SEND_COMP)]  = "send completion event",
    [ucs_ilog2(UCT_IFACE_FLAG_EVENT_RECV)]       = "tag or active message event",
    [ucs_ilog2(UCT_IFACE_FLAG_EVENT_RECV_SIG)]   = "signaled message event",
    [ucs_ilog2(UCT_IFACE_FLAG_PENDING)]          = "pending",
    [ucs_ilog2(UCT_IFACE_FLAG_TAG_EAGER_SHORT)]  = "tag eager short",
    [ucs_ilog2(UCT_IFACE_FLAG_TAG_EAGER_BCOPY)]  = "tag eager bcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_TAG_EAGER_ZCOPY)]  = "tag eager zcopy",
    [ucs_ilog2(UCT_IFACE_FLAG_TAG_RNDV_ZCOPY)]   = "tag rndv zcopy"
};

static ucp_wireup_atomic_flag_t ucp_wireup_atomic_desc[] = {
     [UCT_ATOMIC_OP_ADD]   = {.name = "add",   .fetch = "fetch-"},
     [UCT_ATOMIC_OP_AND]   = {.name = "and",   .fetch = "fetch-"},
     [UCT_ATOMIC_OP_OR]    = {.name = "or",    .fetch = "fetch-"},
     [UCT_ATOMIC_OP_XOR]   = {.name = "xor",   .fetch = "fetch-"},
     [UCT_ATOMIC_OP_SWAP]  = {.name = "swap",  .fetch = ""},
     [UCT_ATOMIC_OP_CSWAP] = {.name = "cscap", .fetch = ""}
};


static double ucp_wireup_aux_score_func(ucp_context_h context,
                                        const uct_md_attr_t *md_attr,
                                        const uct_iface_attr_t *iface_attr,
                                        const ucp_address_iface_attr_t *remote_iface_attr);

static const char *
ucp_wireup_get_missing_flag_desc(uint64_t flags, uint64_t required_flags,
                                 const char ** flag_descs)
{
    ucs_assert((required_flags & (~flags)) != 0);
    return flag_descs[ucs_ffs64(required_flags & (~flags))];
}

static const char *
ucp_wireup_get_missing_amo_flag_desc(uint64_t flags, uint64_t required_flags,
                                     int op_size, int fetch, char *buf, size_t len)
{
    int idx;

    ucs_assert((required_flags & (~flags)) != 0);

    idx = ucs_ffs64(required_flags & (~flags));

    snprintf(buf, len, "%d-bit atomic %s%s", op_size,
             fetch ? ucp_wireup_atomic_desc[idx].fetch : "",
             ucp_wireup_atomic_desc[idx].name);

    return buf;
}

static const char *
ucp_wireup_get_missing_amo_flag_desc_op(uint64_t flags, uint64_t required_flags,
                                        int op_size, char *buf, size_t len)
{
    return ucp_wireup_get_missing_amo_flag_desc(flags, required_flags, op_size, 0, buf, len);
}

static const char *
ucp_wireup_get_missing_amo_flag_desc_fop(uint64_t flags, uint64_t required_flags,
                                         int op_size, char *buf, size_t len)
{
    return ucp_wireup_get_missing_amo_flag_desc(flags, required_flags, op_size, 1, buf, len);
}

static int ucp_wireup_check_flags(const uct_tl_resource_desc_t *resource,
                                  uint64_t flags, uint64_t required_flags,
                                  const char *title, const char ** flag_descs,
                                  char *reason, size_t max)
{
    const char *missing_flag_desc;

    if (ucs_test_all_flags(flags, required_flags)) {
        return 1;
    }

    if (required_flags) {
        missing_flag_desc = ucp_wireup_get_missing_flag_desc(flags, required_flags,
                                                             flag_descs);
        ucs_trace(UCT_TL_RESOURCE_DESC_FMT " : not suitable for %s, no %s",
                  UCT_TL_RESOURCE_DESC_ARG(resource), title,
                  missing_flag_desc);
        snprintf(reason, max, UCT_TL_RESOURCE_DESC_FMT" - no %s",
                 UCT_TL_RESOURCE_DESC_ARG(resource), missing_flag_desc);
    }
    return 0;
}

static int ucp_wireup_check_amo_flags(const uct_tl_resource_desc_t *resource,
                                      uint64_t flags, uint64_t required_flags,
                                      int op_size, int fetch,
                                      const char *title, char *reason, size_t max)
{
    char missing_flag_desc[256];

    if (ucs_test_all_flags(flags, required_flags)) {
        return 1;
    }

    if (required_flags) {
        ucp_wireup_get_missing_amo_flag_desc(flags, required_flags,
                                             op_size, fetch, missing_flag_desc,
                                             sizeof(missing_flag_desc));
        ucs_trace(UCT_TL_RESOURCE_DESC_FMT " : not suitable for %s, no %s",
                  UCT_TL_RESOURCE_DESC_ARG(resource), title,
                  missing_flag_desc);
        snprintf(reason, max, UCT_TL_RESOURCE_DESC_FMT" - no %s",
                 UCT_TL_RESOURCE_DESC_ARG(resource), missing_flag_desc);
    }
    return 0;
}

/**
 * Compare two scores and return:
 * - `-1` if score1 < score2
 * -  `0` if score1 == score2
 * -  `1` if score1 > score2
 */
static int ucp_wireup_score_cmp(double score1, double score2)
{
    double diff = score1 - score2;
    return ((fabs(diff) < ucp_calc_epsilon(score1, score2)) ?
            0 : ucs_signum(diff));
}

/**
 * Select a local and remote transport
 */
static UCS_F_NOINLINE ucs_status_t
ucp_wireup_select_transport(ucp_ep_h ep, const ucp_address_entry_t *address_list,
                            unsigned address_count, const ucp_wireup_criteria_t *criteria,
                            uint64_t tl_bitmap, uint64_t remote_md_map,
                            uint64_t local_dev_bitmap, uint64_t remote_dev_bitmap,
                            int show_error, ucp_wireup_select_info_t *select_info)
{
    ucp_worker_h worker   = ep->worker;
    ucp_context_h context = worker->context;
    uct_tl_resource_desc_t *resource;
    const ucp_address_entry_t *ae;
    ucp_rsc_index_t rsc_index, best_rsc_index;
    double score, best_score;
    char tls_info[256];
    char *p, *endp;
    uct_iface_attr_t *iface_attr;
    uct_md_attr_t *md_attr;
    uint64_t addr_index_map;
    unsigned addr_index, best_dst_addr_index;
    int reachable;
    int found;
    uint8_t priority, best_score_priority;

    found               = 0;
    best_score          = 0.0;
    best_rsc_index      = 0;
    best_dst_addr_index = 0;
    best_score_priority = 0;
    p                   = tls_info;
    endp                = tls_info + sizeof(tls_info) - 1;
    tls_info[0]         = '\0';

    /* Check which remote addresses satisfy the criteria */
    addr_index_map = 0;
    for (ae = address_list; ae < address_list + address_count; ++ae) {
        addr_index = ae - address_list;
        if (!(remote_dev_bitmap & UCS_BIT(ae->dev_index))) {
            ucs_trace("addr[%d]: not in use, because on device[%d]",
                      addr_index, ae->dev_index);
            continue;
        } else if (!(remote_md_map & UCS_BIT(ae->md_index))) {
            ucs_trace("addr[%d]: not in use, because on md[%d]", addr_index,
                      ae->md_index);
            continue;
        } else if (!ucs_test_all_flags(ae->md_flags, criteria->remote_md_flags)) {
            ucs_trace("addr[%d] %s: no %s", addr_index,
                      ucp_find_tl_name_by_csum(context, ae->tl_name_csum),
                      ucp_wireup_get_missing_flag_desc(ae->md_flags,
                                                       criteria->remote_md_flags,
                                                       ucp_wireup_md_flags));
            continue;
        }

        /* Make sure we are indeed passing all flags required by the criteria in
         * ucp packed address */
        ucs_assert(ucs_test_all_flags(UCP_ADDRESS_IFACE_FLAGS,
                                      criteria->remote_iface_flags));

        if (!ucs_test_all_flags(ae->iface_attr.cap_flags, criteria->remote_iface_flags)) {
            ucs_trace("addr[%d] %s: no %s", addr_index,
                      ucp_find_tl_name_by_csum(context, ae->tl_name_csum),
                      ucp_wireup_get_missing_flag_desc(ae->iface_attr.cap_flags,
                                                       criteria->remote_iface_flags,
                                                       ucp_wireup_iface_flags));
            continue;
        }

        UCP_WIREUP_CHECK_AMO_FLAGS(ae, criteria, context, addr_index, op, 32);
        UCP_WIREUP_CHECK_AMO_FLAGS(ae, criteria, context, addr_index, op, 64);
        UCP_WIREUP_CHECK_AMO_FLAGS(ae, criteria, context, addr_index, fop, 32);
        UCP_WIREUP_CHECK_AMO_FLAGS(ae, criteria, context, addr_index, fop, 64);

        addr_index_map |= UCS_BIT(addr_index);
    }

    if (!addr_index_map) {
         snprintf(p, endp - p, "%s  ", ucs_status_string(UCS_ERR_UNSUPPORTED));
         p += strlen(p);
         goto out;
    }

    /* For each local resource try to find the best remote address to connect to.
     * Pick the best local resource to satisfy the criteria.
     * best one has the highest score (from the dedicated score_func) and
     * has a reachable tl on the remote peer */
    ucs_for_each_bit(rsc_index, context->tl_bitmap) {
        resource   = &context->tl_rscs[rsc_index].tl_rsc;
        iface_attr = ucp_worker_iface_get_attr(worker, rsc_index);
        md_attr    = &context->tl_mds[context->tl_rscs[rsc_index].md_index].attr;

        if ((context->tl_rscs[rsc_index].flags & UCP_TL_RSC_FLAG_AUX) &&
            !(criteria->tl_rsc_flags & UCP_TL_RSC_FLAG_AUX)) {
            continue;
        }

        /* Check that local md and interface satisfy the criteria */
        if (!ucp_wireup_check_flags(resource, md_attr->cap.flags,
                                    criteria->local_md_flags, criteria->title,
                                    ucp_wireup_md_flags, p, endp - p) ||
            !ucp_wireup_check_flags(resource, iface_attr->cap.flags,
                                    criteria->local_iface_flags, criteria->title,
                                    ucp_wireup_iface_flags, p, endp - p) ||
            !ucp_wireup_check_amo_flags(resource, iface_attr->cap.atomic32.op_flags,
                                        criteria->local_atomic_flags.atomic32.op_flags,
                                        32, 0, criteria->title, p, endp - p) ||
            !ucp_wireup_check_amo_flags(resource, iface_attr->cap.atomic64.op_flags,
                                        criteria->local_atomic_flags.atomic64.op_flags,
                                        64, 0, criteria->title, p, endp - p) ||
            !ucp_wireup_check_amo_flags(resource, iface_attr->cap.atomic32.fop_flags,
                                        criteria->local_atomic_flags.atomic32.fop_flags,
                                        32, 1, criteria->title, p, endp - p) ||
            !ucp_wireup_check_amo_flags(resource, iface_attr->cap.atomic64.fop_flags,
                                        criteria->local_atomic_flags.atomic64.fop_flags,
                                        64, 1, criteria->title, p, endp - p))
        {
            p += strlen(p);
            snprintf(p, endp - p, ", ");
            p += strlen(p);
            continue;
        }

        /* Check supplied tl & device bitmap */
        if (!(tl_bitmap & UCS_BIT(rsc_index))) {
            ucs_trace(UCT_TL_RESOURCE_DESC_FMT " : disabled by tl_bitmap",
                      UCT_TL_RESOURCE_DESC_ARG(resource));
            snprintf(p, endp - p, UCT_TL_RESOURCE_DESC_FMT" - disabled for %s, ",
                     UCT_TL_RESOURCE_DESC_ARG(resource), criteria->title);
            p += strlen(p);
            continue;
        } else if (!(local_dev_bitmap & UCS_BIT(context->tl_rscs[rsc_index].dev_index))) {
            ucs_trace(UCT_TL_RESOURCE_DESC_FMT " : disabled by device bitmap",
                      UCT_TL_RESOURCE_DESC_ARG(resource));
            snprintf(p, endp - p, UCT_TL_RESOURCE_DESC_FMT" - disabled for %s, ",
                     UCT_TL_RESOURCE_DESC_ARG(resource), criteria->title);
            p += strlen(p);
            continue;
        }

        reachable = 0;

        for (ae = address_list; ae < address_list + address_count; ++ae) {
            if (!(addr_index_map & UCS_BIT(ae - address_list)) ||
                !ucp_wireup_is_reachable(worker, rsc_index, ae))
            {
                /* Must be reachable device address, on same transport */
                continue;
            }

            reachable = 1;

            score = criteria->calc_score(context, md_attr, iface_attr,
                                         &ae->iface_attr);
            ucs_assert(score >= 0.0);

            priority = iface_attr->priority + ae->iface_attr.priority;

            ucs_trace(UCT_TL_RESOURCE_DESC_FMT "->addr[%zd] : %s score %.2f priority %d",
                      UCT_TL_RESOURCE_DESC_ARG(resource), ae - address_list,
                      criteria->title, score, priority);

            if (!found ||
                /* Comparing score with the current best score */
                (ucp_wireup_score_cmp(score, best_score) > 0) ||
                /* Comparing priority with the priority of the current best
                 * transport (if the scores are equal) */
                ((ucp_wireup_score_cmp(score, best_score) == 0) &&
                 (priority > best_score_priority))) {
                best_rsc_index      = rsc_index;
                best_dst_addr_index = ae - address_list;
                best_score          = score;
                best_score_priority = priority;
                found               = 1;
            }
        }

        /* If a local resource cannot reach any of the remote addresses,
         * generate debug message. */
        if (!reachable) {
            snprintf(p, endp - p, UCT_TL_RESOURCE_DESC_FMT" - %s, ",
                     UCT_TL_RESOURCE_DESC_ARG(resource),
                     ucs_status_string(UCS_ERR_UNREACHABLE));
            p += strlen(p);
        }
    }

out:
    if (p >= tls_info + 2) {
        *(p - 2) = '\0'; /* trim last "," */
    }

    select_info->rsc_index  = best_rsc_index;
    select_info->addr_index = best_dst_addr_index;
    select_info->score      = best_score;

    if (!found) {
        if (show_error) {
            ucs_error("no %s transport to %s: %s", criteria->title,
                      ucp_ep_peer_name(ep), tls_info);
        }

        return UCS_ERR_UNREACHABLE;
    }

    ucs_trace("ep %p: selected for %s: " UCT_TL_RESOURCE_DESC_FMT " md[%d]"
              " -> '%s' address[%d],md[%d] score %.2f", ep, criteria->title,
              UCT_TL_RESOURCE_DESC_ARG(&context->tl_rscs[best_rsc_index].tl_rsc),
              context->tl_rscs[best_rsc_index].md_index,
              ucp_ep_peer_name(ep), best_dst_addr_index,
              address_list[best_dst_addr_index].md_index, best_score);

    return UCS_OK;
}

static inline double ucp_wireup_tl_iface_latency(ucp_context_h context,
                                                 const uct_iface_attr_t *iface_attr,
                                                 const ucp_address_iface_attr_t *remote_iface_attr)
{
    return ucs_max(iface_attr->latency.overhead, remote_iface_attr->lat_ovh) +
           (iface_attr->latency.growth * context->config.est_num_eps);
}

static UCS_F_NOINLINE void
ucp_wireup_add_lane_desc(ucp_wireup_select_ctx_t *select_ctx,
                         const ucp_wireup_select_info_t *select_info,
                         ucp_rsc_index_t dst_md_index,
                         uint32_t usage, int is_proxy)
{
    ucp_wireup_lane_desc_t *lane_desc;
    ucp_lane_index_t lane, proxy_lane;
    int proxy_changed;

    /* Add a new lane, but try to reuse already added lanes which are selected
     * on the same transport resources.
     */
    proxy_changed = 0;
    for (lane_desc = select_ctx->lane_descs;
         lane_desc < select_ctx->lane_descs + select_ctx->num_lanes; ++lane_desc) {
        if ((lane_desc->rsc_index == select_info->rsc_index) &&
            (lane_desc->addr_index == select_info->addr_index))
        {
            lane = lane_desc - select_ctx->lane_descs;
            ucs_assertv_always(dst_md_index == lane_desc->dst_md_index,
                               "lane[%d].dst_md_index=%d, dst_md_index=%d",
                               lane, lane_desc->dst_md_index, dst_md_index);
            ucs_assertv_always(!(lane_desc->usage & usage), "lane[%d]=0x%x |= 0x%x",
                               lane, lane_desc->usage, usage);
            if (is_proxy && (lane_desc->proxy_lane == UCP_NULL_LANE)) {
                /* New lane is a proxy, and found existing non-proxy lane with
                 * same resource. So that lane should be used by the proxy.
                 */
                proxy_lane = lane;
                goto out_add_lane;
            } else if (!is_proxy && (lane_desc->proxy_lane == lane)) {
                /* New lane is not a proxy, but found existing proxy lane which
                 * could use the new lane. It also means we should be able to
                 * add our new lane.
                 */
                lane_desc->proxy_lane = select_ctx->num_lanes;
                proxy_changed = 1;
            } else if (!is_proxy && (lane_desc->proxy_lane == UCP_NULL_LANE)) {
                /* Found non-proxy lane with same resource - don't add */
                ucs_assert_always(!proxy_changed);
                lane_desc->usage |= usage;
                goto out_update_score;
            }
        }
    }

    /* If a proxy cannot find other lane with same resource, proxy to self */
    proxy_lane = is_proxy ? select_ctx->num_lanes : UCP_NULL_LANE;

out_add_lane:
    lane_desc = &select_ctx->lane_descs[select_ctx->num_lanes];
    ++select_ctx->num_lanes;

    lane_desc->rsc_index    = select_info->rsc_index;
    lane_desc->addr_index   = select_info->addr_index;
    lane_desc->proxy_lane   = proxy_lane;
    lane_desc->dst_md_index = dst_md_index;
    lane_desc->usage        = usage;
    lane_desc->am_bw_score  = 0.0;
    lane_desc->rma_score    = 0.0;
    lane_desc->rma_bw_score = 0.0;
    lane_desc->amo_score    = 0.0;

out_update_score:
    if (usage & UCP_WIREUP_LANE_USAGE_AM_BW) {
        lane_desc->am_bw_score = select_info->score;
    }
    if (usage & UCP_WIREUP_LANE_USAGE_RMA) {
        lane_desc->rma_score = select_info->score;
    }
    if (usage & UCP_WIREUP_LANE_USAGE_RMA_BW) {
        lane_desc->rma_bw_score = select_info->score;
    }
    if (usage & UCP_WIREUP_LANE_USAGE_AMO) {
        lane_desc->amo_score = select_info->score;
    }
}

#define UCP_WIREUP_COMPARE_SCORE(_elem1, _elem2, _arg, _token) \
    ({ \
        const ucp_lane_index_t *lane1 = (_elem1); \
        const ucp_lane_index_t *lane2 = (_elem2); \
        const ucp_wireup_lane_desc_t *lanes = (_arg); \
        double score1, score2; \
        \
        score1 = (*lane1 == UCP_NULL_LANE) ? 0.0 : lanes[*lane1]._token##_score; \
        score2 = (*lane2 == UCP_NULL_LANE) ? 0.0 : lanes[*lane2]._token##_score; \
        /* sort from highest score to lowest */ \
        (score1 < score2) ? 1 : ((score1 > score2) ? -1 : 0); \
    })

static int ucp_wireup_compare_lane_am_bw_score(const void *elem1, const void *elem2,
                                               void *arg)
{
    return UCP_WIREUP_COMPARE_SCORE(elem1, elem2, arg, am_bw);
}

static int ucp_wireup_compare_lane_rma_score(const void *elem1, const void *elem2,
                                             void *arg)
{
    return UCP_WIREUP_COMPARE_SCORE(elem1, elem2, arg, rma);
}

static int ucp_wireup_compare_lane_rma_bw_score(const void *elem1, const void *elem2,
                                             void *arg)
{
    return UCP_WIREUP_COMPARE_SCORE(elem1, elem2, arg, rma_bw);
}

static int ucp_wireup_compare_lane_amo_score(const void *elem1, const void *elem2,
                                             void *arg)
{
    return UCP_WIREUP_COMPARE_SCORE(elem1, elem2, arg, amo);
}

static uint64_t ucp_wireup_unset_tl_by_md(ucp_ep_h ep, uint64_t tl_bitmap,
                                          ucp_rsc_index_t rsc_index)
{
    ucp_context_h context    = ep->worker->context;
    ucp_rsc_index_t md_index = context->tl_rscs[rsc_index].md_index;
    ucp_rsc_index_t i;

    ucs_for_each_bit(i, context->tl_bitmap) {
        if (context->tl_rscs[i].md_index == md_index) {
            tl_bitmap &= ~UCS_BIT(i);
        }
    }

    return tl_bitmap;
}

static UCS_F_NOINLINE ucs_status_t
ucp_wireup_add_memaccess_lanes(ucp_wireup_select_ctx_t *select_ctx,
                               const ucp_wireup_criteria_t *criteria,
                               uint64_t tl_bitmap, uint32_t usage)
{
    ucp_ep_h ep                          = select_ctx->ep;
    ucp_wireup_criteria_t mem_criteria   = *criteria;
    ucp_wireup_select_info_t select_info = {0};
    int show_error                       = !select_ctx->allow_am;
    ucp_rsc_index_t dst_md_index;
    double reg_score;
    uint64_t remote_md_map;
    ucs_status_t status;
    char title[64];

    remote_md_map = -1;

    /* Select best transport which can reach registered memory */
    snprintf(title, sizeof(title), criteria->title, "registered");
    mem_criteria.title           = title;
    mem_criteria.remote_md_flags = UCT_MD_FLAG_REG | criteria->remote_md_flags;
    status = ucp_wireup_select_transport(ep, select_ctx->address_list,
                                         select_ctx->address_count,
                                         &mem_criteria, tl_bitmap, remote_md_map,
                                         -1, -1, show_error, &select_info);
    if (status != UCS_OK) {
        goto out;
    }

    dst_md_index = select_ctx->address_list[select_info.addr_index].md_index;
    reg_score    = select_info.score;

    /* Add to the list of lanes and remove all occurrences of the remote md
     * from the address list, to avoid selecting the same remote md again. */
    ucp_wireup_add_lane_desc(select_ctx, &select_info, dst_md_index, usage, 0);
    remote_md_map &= ~UCS_BIT(dst_md_index);
    tl_bitmap = ucp_wireup_unset_tl_by_md(ep, tl_bitmap,
                                          select_info.rsc_index);

    /* Select additional transports which can access allocated memory, but
     * only if their scores are better. We need this because a remote memory
     * block can be potentially allocated using one of them, and we might get
     * better performance than the transports which support only registered
     * remote memory. */
    snprintf(title, sizeof(title), criteria->title, "allocated");
    mem_criteria.title           = title;
    mem_criteria.remote_md_flags = UCT_MD_FLAG_ALLOC |
                                   criteria->remote_md_flags;

    while (select_ctx->address_count > 0) {
        status = ucp_wireup_select_transport(ep, select_ctx->address_list,
                                             select_ctx->address_count,
                                             &mem_criteria, tl_bitmap, remote_md_map,
                                             -1, -1, 0, &select_info);
        /* Break if: */
        /* - transport selection wasn't OK */
        if ((status != UCS_OK) ||
            /* - the selected transport is worse than
             *   the transport selected above */
            (ucp_wireup_score_cmp(select_info.score, reg_score) <= 0)) {
            break;
        }

        /* Add lane description and remove all occurrences of the remote md. */
        dst_md_index = select_ctx->address_list[select_info.addr_index].md_index;
        ucp_wireup_add_lane_desc(select_ctx, &select_info, dst_md_index, usage, 0);
        remote_md_map &= ~UCS_BIT(dst_md_index);
        tl_bitmap = ucp_wireup_unset_tl_by_md(ep, tl_bitmap,
                                              select_info.rsc_index);
    }

    status = UCS_OK;

out:
    if ((status != UCS_OK) && select_ctx->allow_am) {
        /* using emulation over active messages */
        select_ctx->ep_init_flags |= UCP_EP_CREATE_AM_LANE;
        status                     = UCS_OK;
    }

    return status;
}

static uint64_t ucp_ep_get_context_features(ucp_ep_h ep)
{
    return ep->worker->context->config.features;
}

static double ucp_wireup_rma_score_func(ucp_context_h context,
                                        const uct_md_attr_t *md_attr,
                                        const uct_iface_attr_t *iface_attr,
                                        const ucp_address_iface_attr_t *remote_iface_attr)
{
    /* best for 4k messages */
    return 1e-3 / (ucp_wireup_tl_iface_latency(context, iface_attr, remote_iface_attr) +
                   iface_attr->overhead +
                   (4096.0 / ucs_min(ucp_tl_iface_bandwidth(context, &iface_attr->bandwidth),
                                     ucp_tl_iface_bandwidth(context, &remote_iface_attr->bandwidth))));
}

static int ucp_wireup_ep_params_is_err_mode_peer(const ucp_ep_params_t *params)
{
    return (params->field_mask & UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE) &&
           (params->err_mode == UCP_ERR_HANDLING_MODE_PEER);
}

static void ucp_wireup_fill_ep_params_criteria(ucp_wireup_criteria_t *criteria,
                                               const ucp_ep_params_t *params)
{
    if (ucp_wireup_ep_params_is_err_mode_peer(params)) {
        criteria->local_iface_flags |= UCT_IFACE_FLAG_ERRHANDLE_PEER_FAILURE;
    }
}

static void ucp_wireup_fill_aux_criteria(ucp_wireup_criteria_t *criteria,
                                         const ucp_ep_params_t *params)
{
    criteria->title              = "auxiliary";
    criteria->local_md_flags     = 0;
    criteria->remote_md_flags    = 0;
    criteria->local_iface_flags  = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                   UCT_IFACE_FLAG_AM_BCOPY |
                                   UCT_IFACE_FLAG_PENDING;
    criteria->remote_iface_flags = UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                   UCT_IFACE_FLAG_AM_BCOPY |
                                   UCT_IFACE_FLAG_CB_ASYNC;
    criteria->calc_score         = ucp_wireup_aux_score_func;
    criteria->tl_rsc_flags       = UCP_TL_RSC_FLAG_AUX; /* Can use aux transports */

    ucp_wireup_fill_ep_params_criteria(criteria, params);
}

static void ucp_wireup_clean_amo_criteria(ucp_wireup_criteria_t *criteria)
{
    memset(&criteria->remote_atomic_flags, 0,
           sizeof(criteria->remote_atomic_flags));
    memset(&criteria->local_atomic_flags, 0,
           sizeof(criteria->local_atomic_flags));
}

static int ucp_wireup_allow_am_emulation_layer(const ucp_ep_params_t *params,
                                               unsigned ep_init_flags)
{
    return !(ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE) &&
           /* disable emulation layer if err handling is required due to lack of
            * keep alive protocol */
           !ucp_wireup_ep_params_is_err_mode_peer(params);
}

static ucs_status_t ucp_wireup_add_rma_lanes(ucp_wireup_select_ctx_t *select_ctx)
{
    ucp_wireup_criteria_t criteria = {0};

    if (!(ucp_ep_get_context_features(select_ctx->ep) & UCP_FEATURE_RMA) &&
        !(select_ctx->ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE)) {
        return UCS_OK;
    }

    if (select_ctx->ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE) {
        criteria.title              = "copy across memory types";
        criteria.remote_iface_flags = UCT_IFACE_FLAG_PUT_SHORT;
        criteria.local_iface_flags  = criteria.remote_iface_flags;
    } else {
        criteria.title              = "remote %s memory access";
        criteria.remote_iface_flags = UCT_IFACE_FLAG_PUT_SHORT |
                                      UCT_IFACE_FLAG_PUT_BCOPY |
                                      UCT_IFACE_FLAG_GET_BCOPY;
        criteria.local_iface_flags  = criteria.remote_iface_flags |
                                      UCT_IFACE_FLAG_PENDING;
    }
    criteria.calc_score             = ucp_wireup_rma_score_func;
    criteria.tl_rsc_flags           = 0;
    ucp_wireup_fill_ep_params_criteria(&criteria, select_ctx->params);

    return ucp_wireup_add_memaccess_lanes(select_ctx, &criteria,
                                          -1, UCP_WIREUP_LANE_USAGE_RMA);
}

double ucp_wireup_amo_score_func(ucp_context_h context,
                                 const uct_md_attr_t *md_attr,
                                 const uct_iface_attr_t *iface_attr,
                                 const ucp_address_iface_attr_t *remote_iface_attr)
{
    /* best one-sided latency */
    return 1e-3 / (ucp_wireup_tl_iface_latency(context, iface_attr, remote_iface_attr) +
                   iface_attr->overhead);
}

static ucs_status_t ucp_wireup_add_amo_lanes(ucp_wireup_select_ctx_t *select_ctx)
{
    ucp_worker_h worker            = select_ctx->ep->worker;
    ucp_context_h context          = worker->context;
    ucp_wireup_criteria_t criteria = {0};
    ucp_rsc_index_t rsc_index;
    uint64_t tl_bitmap;

    if (!ucs_test_flags(context->config.features,
                        UCP_FEATURE_AMO32, UCP_FEATURE_AMO64) ||
        (select_ctx->ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE)) {
        return UCS_OK;
    }

    ucp_context_uct_atomic_iface_flags(context, &criteria.remote_atomic_flags);

    criteria.title              = "atomic operations on %s memory";
    criteria.local_iface_flags  = criteria.remote_iface_flags |
                                  UCT_IFACE_FLAG_PENDING;
    criteria.local_atomic_flags = criteria.remote_atomic_flags;
    criteria.calc_score         = ucp_wireup_amo_score_func;
    ucp_wireup_fill_ep_params_criteria(&criteria, select_ctx->params);

    /* We can use only non-p2p resources or resources which are explicitly
     * selected for atomics. Otherwise, the remote peer would not be able to
     * connect back on p2p transport.
     */
    tl_bitmap = worker->atomic_tls;
    for (rsc_index = 0; rsc_index < context->num_tls; ++rsc_index) {
        if (!ucp_worker_is_tl_p2p(worker, rsc_index)) {
            tl_bitmap |= UCS_BIT(rsc_index);
        }
    }

    return ucp_wireup_add_memaccess_lanes(select_ctx, &criteria,
                                          tl_bitmap, UCP_WIREUP_LANE_USAGE_AMO);
}

static double ucp_wireup_am_score_func(ucp_context_h context,
                                       const uct_md_attr_t *md_attr,
                                       const uct_iface_attr_t *iface_attr,
                                       const ucp_address_iface_attr_t *remote_iface_attr)
{
    /* best end-to-end latency */
    return 1e-3 / (ucp_wireup_tl_iface_latency(context, iface_attr, remote_iface_attr) +
                   iface_attr->overhead + remote_iface_attr->overhead);
}

static double ucp_wireup_rma_bw_score_func(ucp_context_h context,
                                           const uct_md_attr_t *md_attr,
                                           const uct_iface_attr_t *iface_attr,
                                           const ucp_address_iface_attr_t *remote_iface_attr)
{
    /* highest bandwidth with lowest overhead - test a message size of 256KB,
     * a size which is likely to be used for high-bw memory access protocol, for
     * how long it would take to transfer it with a certain transport. */
    return 1 / ((UCP_WIREUP_RMA_BW_TEST_MSG_SIZE /
                ucs_min(ucp_tl_iface_bandwidth(context, &iface_attr->bandwidth),
                        ucp_tl_iface_bandwidth(context, &remote_iface_attr->bandwidth))) +
                ucp_wireup_tl_iface_latency(context, iface_attr, remote_iface_attr) +
                iface_attr->overhead + md_attr->reg_cost.overhead +
                (UCP_WIREUP_RMA_BW_TEST_MSG_SIZE * md_attr->reg_cost.growth));
}

static int ucp_wireup_is_lane_proxy(ucp_ep_h ep, ucp_rsc_index_t rsc_index,
                                    uint64_t remote_cap_flags)
{
    return !ucp_worker_is_tl_p2p(ep->worker, rsc_index) &&
           ((remote_cap_flags & UCP_WORKER_UCT_RECV_EVENT_CAP_FLAGS) ==
            UCT_IFACE_FLAG_EVENT_RECV_SIG);
}

static inline int
ucp_wireup_is_am_required(const ucp_wireup_select_ctx_t *select_ctx)
{
    ucp_ep_h ep = select_ctx->ep;
    ucp_lane_index_t lane;

    /* Check if we need active messages from the configurations, for wireup.
     * If not, check if am is required due to p2p transports */

    if ((select_ctx->ep_init_flags & UCP_EP_CREATE_AM_LANE) ||
        (select_ctx->params->field_mask & UCP_EP_PARAM_FIELD_SOCK_ADDR)) {
        return 1;
    }

    if (!(select_ctx->ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE) &&
        (ucp_ep_get_context_features(ep) & (UCP_FEATURE_TAG | 
                                            UCP_FEATURE_STREAM | 
                                            UCP_FEATURE_AM))) {
        return 1;
    }

    for (lane = 0; lane < select_ctx->num_lanes; ++lane) {
        if (ucp_worker_is_tl_p2p(ep->worker,
                                 select_ctx->lane_descs[lane].rsc_index)) {
            return 1;
        }
    }

    return 0;
}

static ucs_status_t ucp_wireup_add_am_lane(ucp_wireup_select_ctx_t *select_ctx)
{
    ucp_ep_h ep                    = select_ctx->ep;
    ucp_wireup_criteria_t criteria = {0};
    ucp_wireup_select_info_t *am_info;
    uint64_t remote_cap_flags;
    ucp_rsc_index_t dst_md_index;
    ucs_status_t status;
    int is_proxy;

    if (!ucp_wireup_is_am_required(select_ctx)) {
        return UCS_OK;
    }

    /* Select one lane for active messages */
    criteria.title              = "active messages";
    criteria.remote_iface_flags = UCT_IFACE_FLAG_AM_BCOPY |
                                  UCT_IFACE_FLAG_CB_SYNC;
    criteria.local_iface_flags  = UCT_IFACE_FLAG_AM_BCOPY;
    criteria.calc_score         = ucp_wireup_am_score_func;
    ucp_wireup_fill_ep_params_criteria(&criteria, select_ctx->params);

    if (ucs_test_all_flags(ucp_ep_get_context_features(ep),
                           UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP)) {
        criteria.local_iface_flags |= UCP_WORKER_UCT_UNSIG_EVENT_CAP_FLAGS;
    }

    status = ucp_wireup_select_transport(ep, select_ctx->address_list,
                                         select_ctx->address_count, &criteria,
                                         -1, -1, -1, -1, 1, &select_ctx->am_info);
    if (status != UCS_OK) {
        return status;
    }

    am_info      = &select_ctx->am_info;
    dst_md_index = select_ctx->address_list[am_info->addr_index].md_index;

    /* If the remote side is not p2p and has only signaled-am wakeup, it may
     * deactivate its interface and wait for signaled active message to wake up.
     * Use a proxy lane which would send the first active message as signaled to
     * make sure the remote interface will indeed wake up. */
    remote_cap_flags = select_ctx->address_list[am_info->addr_index].iface_attr.cap_flags;
    is_proxy         = ucp_wireup_is_lane_proxy(ep, am_info->rsc_index,
                                                remote_cap_flags);

    ucp_wireup_add_lane_desc(select_ctx, am_info, dst_md_index,
                             UCP_WIREUP_LANE_USAGE_AM, is_proxy);

    return UCS_OK;
}

static double ucp_wireup_am_bw_score_func(ucp_context_h context,
                                          const uct_md_attr_t *md_attr,
                                          const uct_iface_attr_t *iface_attr,
                                          const ucp_address_iface_attr_t *remote_iface_attr)
{
    /* best single MTU bandwidth */
    double size = iface_attr->cap.am.max_bcopy;
    double time = (size / ucs_min(ucp_tl_iface_bandwidth(context, &iface_attr->bandwidth),
                                  ucp_tl_iface_bandwidth(context, &remote_iface_attr->bandwidth))) +
                  iface_attr->overhead + remote_iface_attr->overhead +
                  ucp_wireup_tl_iface_latency(context, iface_attr, remote_iface_attr);

    return size / time * 1e-5;
}

int ucp_wireup_is_rsc_self_or_shm(ucp_ep_h ep, ucp_rsc_index_t rsc_index)
{
    return (ep->worker->context->tl_rscs[rsc_index].tl_rsc.dev_type == UCT_DEVICE_TYPE_SHM) ||
           (ep->worker->context->tl_rscs[rsc_index].tl_rsc.dev_type == UCT_DEVICE_TYPE_SELF);
}

static ucs_status_t
ucp_wireup_add_bw_lanes(ucp_wireup_select_ctx_t *select_ctx,
                        const ucp_wireup_select_bw_info_t *bw_info,
                        int allow_proxy, uint64_t tl_bitmap)
{
    ucp_ep_h ep                          = select_ctx->ep;
    ucp_context_h context                = ep->worker->context;
    ucp_wireup_select_info_t select_info = {0};
    ucs_status_t status;
    int num_lanes;
    ucp_rsc_index_t dst_md_index;
    uint64_t local_dev_bitmap;
    uint64_t remote_dev_bitmap;
    uint64_t remote_cap_flags;
    ucp_md_map_t md_map;
    int is_proxy;

    num_lanes         = 0;
    md_map            = bw_info->md_map;
    local_dev_bitmap  = bw_info->local_dev_bitmap;
    remote_dev_bitmap = bw_info->remote_dev_bitmap;

    /* lookup for requested number of lanes or limit of MD map
     * (we have to limit MD's number to avoid malloc in
     * memory registration) */
    while ((num_lanes < bw_info->max_lanes) &&
           (ucs_popcount(md_map) < UCP_MAX_OP_MDS)) {
        status = ucp_wireup_select_transport(ep, select_ctx->address_list,
                                             select_ctx->address_count,
                                             &bw_info->criteria, tl_bitmap, -1,
                                             local_dev_bitmap, remote_dev_bitmap,
                                             0, &select_info);
        if (status != UCS_OK) {
            break;
        }

        remote_cap_flags = select_ctx->address_list[select_info.addr_index].iface_attr.cap_flags;
        is_proxy         = (allow_proxy &&
                            ucp_wireup_is_lane_proxy(ep, select_info.rsc_index,
                                                     remote_cap_flags));
        dst_md_index     = select_ctx->address_list[select_info.addr_index].md_index;

        ucp_wireup_add_lane_desc(select_ctx, &select_info, dst_md_index,
                                 bw_info->usage, is_proxy);
        md_map |= UCS_BIT(context->tl_rscs[select_info.rsc_index].md_index);
        num_lanes++;

        local_dev_bitmap  &= ~UCS_BIT(context->tl_rscs[select_info.rsc_index].dev_index);
        remote_dev_bitmap &= ~UCS_BIT(select_ctx->address_list[select_info.addr_index].dev_index);

        if (ucp_wireup_is_rsc_self_or_shm(ep, select_info.rsc_index)) {
            /* special case for SHM: do not try to lookup additional lanes when
             * SHM transport detected (another transport will be significantly
             * slower) */
            break;
        }
    }

    return UCS_OK;
}

static ucs_status_t
ucp_wireup_add_am_bw_lanes(ucp_wireup_select_ctx_t *select_ctx)
{
    ucp_ep_h ep           = select_ctx->ep; 
    ucp_context_h context = ep->worker->context;
    ucp_wireup_select_bw_info_t bw_info;
    ucp_lane_index_t lane_desc_idx;
    ucp_rsc_index_t rsc_index;
    unsigned addr_index;

    /* Check if we need active messages, for wireup */
    if (!(ucp_ep_get_context_features(ep) & UCP_FEATURE_TAG) ||
        (select_ctx->ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE) ||
        (context->config.ext.max_eager_lanes < 2)) {
        return UCS_OK;
    }

    /* Select one lane for active messages */
    bw_info.criteria.title              = "high-bw active messages";
    bw_info.criteria.local_md_flags     = 0;
    bw_info.criteria.remote_md_flags    = 0;
    bw_info.criteria.remote_iface_flags = UCT_IFACE_FLAG_AM_BCOPY |
                                          UCT_IFACE_FLAG_CB_SYNC;
    bw_info.criteria.local_iface_flags  = UCT_IFACE_FLAG_AM_BCOPY;
    bw_info.criteria.calc_score         = ucp_wireup_am_bw_score_func;
    bw_info.criteria.tl_rsc_flags       = 0;
    ucp_wireup_clean_amo_criteria(&bw_info.criteria);
    ucp_wireup_fill_ep_params_criteria(&bw_info.criteria,
                                       select_ctx->params);

    if (ucs_test_all_flags(ucp_ep_get_context_features(ep),
                           UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP)) {
        bw_info.criteria.local_iface_flags |= UCP_WORKER_UCT_UNSIG_EVENT_CAP_FLAGS;
    }

    bw_info.local_dev_bitmap  = -1;
    bw_info.remote_dev_bitmap = -1;
    bw_info.md_map            = 0;
    bw_info.max_lanes         = context->config.ext.max_eager_lanes - 1;
    bw_info.usage             = UCP_WIREUP_LANE_USAGE_AM_BW;

    /* am_bw_lane[0] is am_lane, so don't re-select it here */
    for (lane_desc_idx = 0; lane_desc_idx < select_ctx->num_lanes; ++lane_desc_idx) {
        if (select_ctx->lane_descs[lane_desc_idx].usage & UCP_WIREUP_LANE_USAGE_AM) {
            addr_index                 = select_ctx->lane_descs[lane_desc_idx].addr_index;
            rsc_index                  = select_ctx->lane_descs[lane_desc_idx].rsc_index;
            bw_info.md_map            |= UCS_BIT(context->tl_rscs[rsc_index].md_index);
            bw_info.local_dev_bitmap  &= ~UCS_BIT(context->tl_rscs[rsc_index].dev_index);
            bw_info.remote_dev_bitmap &= ~UCS_BIT(select_ctx->address_list[addr_index].dev_index);
            if (ucp_wireup_is_rsc_self_or_shm(ep, rsc_index)) {
                /* if AM lane is SELF or SHMEM - then do not use more lanes */
                return UCS_OK;
            } else {
                break; /* do not continue searching due to we found
                          AM lane (and there is only one lane) */
            }
        }
    }

    return ucp_wireup_add_bw_lanes(select_ctx, &bw_info, 1, -1);
}

static ucs_status_t
ucp_wireup_add_rma_bw_lanes(ucp_wireup_select_ctx_t *select_ctx)
{
    ucp_ep_h ep           = select_ctx->ep;
    ucp_context_h context = ep->worker->context;
    ucp_wireup_select_bw_info_t bw_info;
    ucs_memory_type_t mem_type;

    if (select_ctx->ep_init_flags & UCP_EP_INIT_FLAG_MEM_TYPE) {
        bw_info.criteria.remote_md_flags = 0;
        bw_info.criteria.local_md_flags  = 0;
    } else if (ucp_ep_get_context_features(ep) & UCP_FEATURE_TAG) {
        /* if needed for RNDV, need only access for remote registered memory */
        bw_info.criteria.remote_md_flags = UCT_MD_FLAG_REG;
        bw_info.criteria.local_md_flags  = UCT_MD_FLAG_REG;
    } else {
        return UCS_OK;
    }

    bw_info.criteria.title              = "high-bw remote memory access";
    bw_info.criteria.remote_iface_flags = UCT_IFACE_FLAG_GET_ZCOPY |
                                          UCT_IFACE_FLAG_PUT_ZCOPY;
    bw_info.criteria.local_iface_flags  = bw_info.criteria.remote_iface_flags |
                                          UCT_IFACE_FLAG_PENDING;
    bw_info.criteria.calc_score         = ucp_wireup_rma_bw_score_func;
    bw_info.criteria.tl_rsc_flags       = 0;
    ucp_wireup_clean_amo_criteria(&bw_info.criteria);
    ucp_wireup_fill_ep_params_criteria(&bw_info.criteria,
                                       select_ctx->params);

    if (ucs_test_all_flags(ucp_ep_get_context_features(ep),
                           UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP)) {
        bw_info.criteria.local_iface_flags |= UCP_WORKER_UCT_UNSIG_EVENT_CAP_FLAGS;
    }

    bw_info.local_dev_bitmap  = -1;
    bw_info.remote_dev_bitmap = -1;
    bw_info.md_map            = 0;
    bw_info.max_lanes         = context->config.ext.max_rndv_lanes;
    bw_info.usage             = UCP_WIREUP_LANE_USAGE_RMA_BW;

    for (mem_type = 0; mem_type < UCS_MEMORY_TYPE_LAST; mem_type++) {
        if (!context->mem_type_access_tls[mem_type]) {
            continue;
        }

        ucp_wireup_add_bw_lanes(select_ctx, &bw_info, 0,
                                context->mem_type_access_tls[mem_type]);
    }

    return UCS_OK;
}

/* Lane for transport offloaded tag interface */
static ucs_status_t
ucp_wireup_add_tag_lane(ucp_wireup_select_ctx_t *select_ctx,
                        ucp_err_handling_mode_t err_mode)
{
    ucp_ep_h ep                          = select_ctx->ep;
    ucp_wireup_criteria_t criteria       = {0};
    ucp_wireup_select_info_t select_info = {0};
    uint64_t remote_cap_flags;
    ucp_rsc_index_t dst_md_index;
    ucs_status_t status;
    int is_proxy;

    if (!(ucp_ep_get_context_features(ep) & UCP_FEATURE_TAG) ||
        /* TODO: remove check below when UCP_ERR_HANDLING_MODE_PEER supports
         *       RNDV-protocol or HW TM supports fragmented protocols
         */
        (err_mode != UCP_ERR_HANDLING_MODE_NONE)) {
        return UCS_OK;
    }

    criteria.title              = "tag_offload";
    criteria.local_md_flags     = UCT_MD_FLAG_REG; /* needed for posting tags to HW */
    criteria.remote_md_flags    = UCT_MD_FLAG_REG; /* needed for posting tags to HW */
    criteria.remote_iface_flags = /* the same as local_iface_flags */
    criteria.local_iface_flags  = UCT_IFACE_FLAG_TAG_EAGER_BCOPY |
                                  UCT_IFACE_FLAG_TAG_RNDV_ZCOPY  |
                                  UCT_IFACE_FLAG_GET_ZCOPY       |
                                  UCT_IFACE_FLAG_PENDING;
    criteria.calc_score         = ucp_wireup_am_score_func;

    if (ucs_test_all_flags(ucp_ep_get_context_features(ep),
                           UCP_FEATURE_WAKEUP)) {
        criteria.local_iface_flags |= UCP_WORKER_UCT_UNSIG_EVENT_CAP_FLAGS;
    }

    /* Do not add tag offload lane, if selected tag lane score is lower
     * than AM score. In this case AM will be used for tag macthing. */
    status = ucp_wireup_select_transport(ep, select_ctx->address_list,
                                         select_ctx->address_count,
                                         &criteria, -1, -1, -1, -1, 0,
                                         &select_info);
    /* Break if: */
    /* - transport selection wasn't OK */
    if ((status != UCS_OK) ||
        /* - the TAG transport is worse than the AM transport */
        (ucp_wireup_score_cmp(select_info.score,
                              select_ctx->am_info.score) < 0)) {
        goto out;
    }

    /* If the remote side is not p2p and has only signaled wakeup, it may
     * deactivate its interface and wait for signaled tag message to wake up.
     * Use a proxy lane which would send the first tag message as signaled to
     * make sure the remote interface will indeed wake up. */
    remote_cap_flags = select_ctx->address_list[select_info.addr_index].iface_attr.cap_flags;
    is_proxy         = ucp_wireup_is_lane_proxy(ep, select_info.rsc_index,
                                                remote_cap_flags);
    dst_md_index     = select_ctx->address_list[select_info.addr_index].md_index;

    ucp_wireup_add_lane_desc(select_ctx, &select_info, dst_md_index,
                             UCP_WIREUP_LANE_USAGE_TAG, is_proxy);

out:
    return UCS_OK;
}

static ucp_lane_index_t
ucp_wireup_select_wireup_msg_lane(ucp_worker_h worker,
                                  const ucp_ep_params_t *ep_params,
                                  const ucp_address_entry_t *address_list,
                                  const ucp_wireup_lane_desc_t *lane_descs,
                                  ucp_lane_index_t num_lanes)
{
    ucp_context_h context          = worker->context;
    ucp_lane_index_t p2p_lane      = UCP_NULL_LANE;
    ucp_wireup_criteria_t criteria = {0};
    uct_tl_resource_desc_t *resource;
    ucp_rsc_index_t rsc_index;
    uct_iface_attr_t *attrs;
    ucp_lane_index_t lane;
    unsigned addr_index;

    ucp_wireup_fill_aux_criteria(&criteria, ep_params);
    for (lane = 0; lane < num_lanes; ++lane) {
        rsc_index  = lane_descs[lane].rsc_index;
        addr_index = lane_descs[lane].addr_index;
        resource   = &context->tl_rscs[rsc_index].tl_rsc;
        attrs      = ucp_worker_iface_get_attr(worker, rsc_index);

        /* if the current lane satisfies the wireup criteria, choose it for wireup.
         * if it doesn't take a lane with a p2p transport */
        if (ucp_wireup_check_flags(resource,
                                   attrs->cap.flags,
                                   criteria.local_iface_flags, criteria.title,
                                   ucp_wireup_iface_flags, NULL, 0) &&
            ucp_wireup_check_flags(resource,
                                   address_list[addr_index].iface_attr.cap_flags,
                                   criteria.remote_iface_flags, criteria.title,
                                   ucp_wireup_iface_flags, NULL, 0))
         {
             return lane;
         } else if (ucp_worker_is_tl_p2p(worker, rsc_index)) {
             p2p_lane = lane;
         }
    }

    return p2p_lane;
}

static UCS_F_NOINLINE void
ucp_wireup_select_ctx_init(ucp_wireup_select_ctx_t *select_ctx,
                           ucp_ep_h ep, const ucp_ep_params_t *params,
                           unsigned ep_init_flags, unsigned address_count,
                           const ucp_address_entry_t *address_list)
{
    select_ctx->ep                 = ep;
    select_ctx->params             = params;
    select_ctx->ep_init_flags      = ep_init_flags;
    select_ctx->address_list       = address_list;
    select_ctx->address_count      = address_count;
    select_ctx->num_lanes          = 0;
    select_ctx->allow_am           =
        ucp_wireup_allow_am_emulation_layer(params, ep_init_flags);
    memset(select_ctx->lane_descs, 0, sizeof(select_ctx->lane_descs));
}

static UCS_F_NOINLINE ucs_status_t
ucp_wireup_search_lanes(ucp_wireup_select_ctx_t *select_ctx,
                        const ucp_ep_config_key_t *key)
{
    ucs_status_t status;

    status = ucp_wireup_add_rma_lanes(select_ctx);
    if (status != UCS_OK) {
        return status;
    }

    status = ucp_wireup_add_amo_lanes(select_ctx);
    if (status != UCS_OK) {
        return status;
    }

    status = ucp_wireup_add_am_lane(select_ctx);
    if (status != UCS_OK) {
        return status;
    }

    status = ucp_wireup_add_rma_bw_lanes(select_ctx);
    if (status != UCS_OK) {
        return status;
    }

    status = ucp_wireup_add_tag_lane(select_ctx, key->err_mode);
    if (status != UCS_OK) {
        return status;
    }

    /* call ucp_wireup_add_am_bw_lanes after ucp_wireup_add_am_lane to
     * allow exclude AM lane from AM_BW list */
    status = ucp_wireup_add_am_bw_lanes(select_ctx);
    if (status != UCS_OK) {
        return status;
    }

    /* User should not create endpoints unless requested communication features */
    if (select_ctx->num_lanes == 0) {
        ucs_error("No transports selected to %s (features: 0x%lx)",
                  ucp_ep_peer_name(select_ctx->ep),
                  ucp_ep_get_context_features(select_ctx->ep));
        return UCS_ERR_UNREACHABLE;
    }

    return UCS_OK;
}

static UCS_F_NOINLINE void
ucp_wireup_construct_lanes(ucp_wireup_select_ctx_t *select_ctx,
                           uint8_t *addr_indices, ucp_ep_config_key_t *key)
{
    ucp_ep_h ep           = select_ctx->ep;
    ucp_worker_h worker   = ep->worker;
    ucp_context_h context = worker->context;
    ucp_rsc_index_t rsc_index;
    ucp_md_index_t md_index;
    ucp_lane_index_t lane;
    ucp_lane_index_t i;

    key->num_lanes = select_ctx->num_lanes;
    /* Construct the endpoint configuration key:
     * - arrange lane description in the EP configuration
     * - create remote MD bitmap
     * - if AM lane exists and fits for wireup messages, select it for this purpose.
     */
    for (lane = 0; lane < key->num_lanes; ++lane) {
        ucs_assert(select_ctx->lane_descs[lane].usage != 0);
        key->lanes[lane].rsc_index    = select_ctx->lane_descs[lane].rsc_index;
        key->lanes[lane].proxy_lane   = select_ctx->lane_descs[lane].proxy_lane;
        key->lanes[lane].dst_md_index = select_ctx->lane_descs[lane].dst_md_index;
        addr_indices[lane]            = select_ctx->lane_descs[lane].addr_index;

        if (select_ctx->lane_descs[lane].usage & UCP_WIREUP_LANE_USAGE_AM) {
            ucs_assert(key->am_lane == UCP_NULL_LANE);
            key->am_lane = lane;
        }
        if ((select_ctx->lane_descs[lane].usage & UCP_WIREUP_LANE_USAGE_AM_BW) &&
            (lane < UCP_MAX_LANES - 1)) {
            key->am_bw_lanes[lane + 1] = lane;
        }
        if (select_ctx->lane_descs[lane].usage & UCP_WIREUP_LANE_USAGE_RMA) {
            key->rma_lanes[lane] = lane;
        }
        if (select_ctx->lane_descs[lane].usage & UCP_WIREUP_LANE_USAGE_RMA_BW) {
            key->rma_bw_lanes[lane] = lane;
        }
        if (select_ctx->lane_descs[lane].usage & UCP_WIREUP_LANE_USAGE_AMO) {
            key->amo_lanes[lane] = lane;
        }
        if (select_ctx->lane_descs[lane].usage & UCP_WIREUP_LANE_USAGE_TAG) {
            ucs_assert(key->tag_lane == UCP_NULL_LANE);
            key->tag_lane = lane;
        }
    }

    /* Sort AM, RMA and AMO lanes according to score */
    ucs_qsort_r(key->am_bw_lanes + 1, UCP_MAX_LANES - 1, sizeof(ucp_lane_index_t),
                ucp_wireup_compare_lane_am_bw_score, select_ctx->lane_descs);
    ucs_qsort_r(key->rma_lanes, UCP_MAX_LANES, sizeof(ucp_lane_index_t),
                ucp_wireup_compare_lane_rma_score, select_ctx->lane_descs);
    ucs_qsort_r(key->rma_bw_lanes, UCP_MAX_LANES, sizeof(ucp_lane_index_t),
                ucp_wireup_compare_lane_rma_bw_score, select_ctx->lane_descs);
    ucs_qsort_r(key->amo_lanes, UCP_MAX_LANES, sizeof(ucp_lane_index_t),
                ucp_wireup_compare_lane_amo_score, select_ctx->lane_descs);

    /* Select lane for wireup messages */
    key->wireup_lane =
        ucp_wireup_select_wireup_msg_lane(worker, select_ctx->params,
                                          select_ctx->address_list,
                                          select_ctx->lane_descs,
                                          key->num_lanes);

    /* add to map first UCP_MAX_OP_MDS fastest MD's */
    for (i = 0;
         (key->rma_bw_lanes[i] != UCP_NULL_LANE) &&
         (ucs_popcount(key->rma_bw_md_map) < UCP_MAX_OP_MDS); i++) {
        lane = key->rma_bw_lanes[i];
        rsc_index = select_ctx->lane_descs[lane].rsc_index;
        md_index  = context->tl_rscs[rsc_index].md_index;

        /* Pack remote key only if needed for RMA.
         * FIXME a temporary workaround to prevent the ugni uct from using rndv. */
        if ((context->tl_mds[md_index].attr.cap.flags & UCT_MD_FLAG_NEED_RKEY) &&
            !(strstr(context->tl_rscs[rsc_index].tl_rsc.tl_name, "ugni"))) {
            key->rma_bw_md_map |= UCS_BIT(md_index);
        }
    }

    /* use AM lane first for eager AM transport to simplify processing single/middle
     * msg packets */
    key->am_bw_lanes[0] = key->am_lane;
}

ucs_status_t ucp_wireup_select_lanes(ucp_ep_h ep, const ucp_ep_params_t *params,
                                     unsigned ep_init_flags, unsigned address_count,
                                     const ucp_address_entry_t *address_list,
                                     uint8_t *addr_indices, ucp_ep_config_key_t *key)
{
    ucp_wireup_select_ctx_t select_ctx;
    ucs_status_t status;

    ucp_wireup_select_ctx_init(&select_ctx, ep, params, ep_init_flags,
                               address_count, address_list);

    status = ucp_wireup_search_lanes(&select_ctx, key);
    if (status != UCS_OK) {
        return status;
    }

    ucp_wireup_construct_lanes(&select_ctx, addr_indices, key);

    return UCS_OK;
}

static double ucp_wireup_aux_score_func(ucp_context_h context,
                                        const uct_md_attr_t *md_attr,
                                        const uct_iface_attr_t *iface_attr,
                                        const ucp_address_iface_attr_t *remote_iface_attr)
{
    /* best end-to-end latency and larger bcopy size */
    return (1e-3 / (ucp_wireup_tl_iface_latency(context, iface_attr, remote_iface_attr) +
            iface_attr->overhead + remote_iface_attr->overhead));
}

ucs_status_t ucp_wireup_select_aux_transport(ucp_ep_h ep,
                                             const ucp_ep_params_t *params,
                                             const ucp_address_entry_t *address_list,
                                             unsigned address_count,
                                             ucp_wireup_select_info_t *select_info)
{
    ucp_wireup_criteria_t criteria = {0};

    ucp_wireup_fill_aux_criteria(&criteria, params);
    return ucp_wireup_select_transport(ep, address_list, address_count,
                                       &criteria, -1, -1, -1, -1, 1,
                                       select_info);
}

ucs_status_t ucp_wireup_select_sockaddr_transport(ucp_ep_h ep,
                                                  const ucp_ep_params_t *params,
                                                  ucp_rsc_index_t *rsc_index_p)
{
    ucp_worker_h worker   = ep->worker;
    ucp_context_h context = worker->context;
    char saddr_str[UCS_SOCKADDR_STRING_LEN];
    ucp_tl_resource_desc_t *resource;
    ucp_rsc_index_t tl_id;
    ucp_md_index_t md_index;
    uct_md_h md;
    int i;

    /* Go over the sockaddr transports priority array and try to use the transports
     * one by one for the client side */
    for (i = 0; i < context->config.num_sockaddr_tls; i++) {
        tl_id    = context->config.sockaddr_tl_ids[i];
        resource = &context->tl_rscs[tl_id];
        md_index = resource->md_index;
        md       = context->tl_mds[md_index].md;

        ucs_assert(context->tl_mds[md_index].attr.cap.flags &
                   UCT_MD_FLAG_SOCKADDR);

        /* The client selects the transport for sockaddr according to the
         * configuration. We rely on the server having this transport available
         * as well */
        if (uct_md_is_sockaddr_accessible(md, &params->sockaddr,
                                          UCT_SOCKADDR_ACC_REMOTE)) {
            *rsc_index_p = tl_id;
            ucs_debug("sockaddr transport selected: %s", resource->tl_rsc.tl_name);
            return UCS_OK;
        }

        ucs_debug("md %s cannot reach %s",
                  context->tl_mds[md_index].rsc.md_name,
                  ucs_sockaddr_str(params->sockaddr.addr, saddr_str,
                                   sizeof(saddr_str)));
    }

    return UCS_ERR_UNREACHABLE;
}
