/*
 * Copyright (c) 2026, Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * NVSHMEM libfabric transport LTTng tracepoint definitions.
 *
 * Naming convention: {side}_{action}_{what}
 *   side:   sender_ or receiver_
 *   action: post_, completion_, gdrcopy_
 *   what:   rma, signal, amo, signal_ack, amo_ack, amo_response
 */

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER nvshmem_libfabric

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./nvshmem_libfabric_trace.h"

#if !defined(_NVSHMEM_LIBFABRIC_TRACE_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _NVSHMEM_LIBFABRIC_TRACE_H

#include <lttng/tracepoint.h>
#include <stdint.h>

/* ========================================================================
 * Sender posts
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, sender_post_rma,
    LTTNG_UST_TP_ARGS(int, pe, int, domain_index, uint64_t, op_size, int, verb_desc, uint64_t, remote_addr),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, pe, pe)
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint64_t, op_size, op_size)
        lttng_ust_field_integer(int, verb_desc, verb_desc)
        lttng_ust_field_integer(uint64_t, remote_addr, remote_addr)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, sender_post_signal,
    LTTNG_UST_TP_ARGS(int, pe, int, domain_index, uint32_t, sequence_count, uint16_t, num_writes),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, pe, pe)
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint32_t, sequence_count, sequence_count)
        lttng_ust_field_integer(uint16_t, num_writes, num_writes)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, sender_post_amo,
    LTTNG_UST_TP_ARGS(int, pe, int, domain_index, int, op_type),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, pe, pe)
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, op_type, op_type)
    )
)

/* ========================================================================
 * FI_REMOTE_CQ_DATA completions (remote fi_writedata arrived)
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, write_remote_completion,
    LTTNG_UST_TP_ARGS(int, domain_index, uint64_t, addr, int, imm_header),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint64_t, addr, addr)
        lttng_ust_field_integer(int, imm_header, imm_header)
    )
)

/* ========================================================================
 * FI_RMA completions (local fi_writedata completed)
 * type: RMA(3)=sender put
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, write_completion,
    LTTNG_UST_TP_ARGS(int, domain_index, uint64_t, wr_id, int, type),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint64_t, wr_id, wr_id)
        lttng_ust_field_integer(int, type, type)
    )
)

/* ========================================================================
 * FI_SEND completions (local fi_send completed)
 * type: SEND(0)=sender AMO, ACK(1)=receiver AMO response, MATCH(2)=sender signal,
 *       AMO_ACK_SEND(3)=AMO ack send
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, send_completion,
    LTTNG_UST_TP_ARGS(int, domain_index, uint64_t, wr_id, int, type),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint64_t, wr_id, wr_id)
        lttng_ust_field_integer(int, type, type)
    )
)

/* ========================================================================
 * FI_RECV completions (remote fi_send arrived)
 * type: SEND(0)=AMO at receiver, ACK(1)=AMO response at sender, MATCH(2)=signal at receiver,
 *       AMO_ACK_SEND(3)=AMO ack receive at sender
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, recv_completion,
    LTTNG_UST_TP_ARGS(int, domain_index, uint64_t, addr, int, type),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint64_t, addr, addr)
        lttng_ust_field_integer(int, type, type)
    )
)

/* ========================================================================
 * Receiver GDRCopy execution (duration event on Thread B)
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, receiver_gdrcopy_start,
    LTTNG_UST_TP_ARGS(int, domain_index, int, op_type, int, is_signal_origin),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, op_type, op_type)
        lttng_ust_field_integer(int, is_signal_origin, is_signal_origin)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, receiver_gdrcopy_end,
    LTTNG_UST_TP_ARGS(int, domain_index, int, status),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, status, status)
    )
)

/* ========================================================================
 * Receiver ack/response posts
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, receiver_post_ack,
    LTTNG_UST_TP_ARGS(int, pe, int, domain_index, uint32_t, sequence_count, int, ack_header),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, pe, pe)
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(uint32_t, sequence_count, sequence_count)
        lttng_ust_field_integer(int, ack_header, ack_header)
    )
)



/* ========================================================================
 * Outstanding counter
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, outstanding,
    LTTNG_UST_TP_ARGS(int, domain_index, int64_t, count),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int64_t, count, count)
    )
)

/* ========================================================================
 * Progress duration (disabled by default)
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, progress_start,
    LTTNG_UST_TP_ARGS(int, domain_index, int, qp_index),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, qp_index, qp_index)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, progress_end,
    LTTNG_UST_TP_ARGS(int, domain_index, int, status),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, status, status)
    )
)

/* ========================================================================
 * Unused — defined but never called
 * ======================================================================== */

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, signal_delivered,
    LTTNG_UST_TP_ARGS(int, domain_index, int, pe, uint32_t, seq_num),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, pe, pe)
        lttng_ust_field_integer(uint32_t, seq_num, seq_num)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, gdr_process_ack_start,
    LTTNG_UST_TP_ARGS(int, domain_index),
    LTTNG_UST_TP_FIELDS(lttng_ust_field_integer(int, domain_index, domain_index))
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, gdr_process_ack_end,
    LTTNG_UST_TP_ARGS(int, domain_index, int, status),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, status, status)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, gdr_process_amo_start,
    LTTNG_UST_TP_ARGS(int, domain_index),
    LTTNG_UST_TP_FIELDS(lttng_ust_field_integer(int, domain_index, domain_index))
)

LTTNG_UST_TRACEPOINT_EVENT(nvshmem_libfabric, gdr_process_amo_end,
    LTTNG_UST_TP_ARGS(int, domain_index, int, status),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(int, domain_index, domain_index)
        lttng_ust_field_integer(int, status, status)
    )
)

#endif /* _NVSHMEM_LIBFABRIC_TRACE_H */

#include <lttng/tracepoint-event.h>
