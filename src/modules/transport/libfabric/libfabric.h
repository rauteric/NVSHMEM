/*
 * Copyright (c) 2016-2025, NVIDIA CORPORATION. All rights reserved.
 *
 * See License.txt for license information
 */

#include <assert.h>
#include <atomic>
#include <pthread.h>
#include <stdint.h>  // IWYU pragma: keep
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <deque>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "rdma/fabric.h"

// IWYU pragma: no_include <bits/stdint-uintn.h>

#include "non_abi/nvshmem_build_options.h"
#include "device_host_transport/nvshmem_common_transport.h"

#ifdef NVSHMEM_USE_GDRCOPY
#include "gdrapi.h"
#endif

#define NVSHMEMT_LIBFABRIC_MAJ_VER 1
#define NVSHMEMT_LIBFABRIC_MIN_VER 5

#define NVSHMEMT_LIBFABRIC_DOMAIN_LEN 32
#define NVSHMEMT_LIBFABRIC_PROVIDER_LEN 32
#define NVSHMEMT_LIBFABRIC_EP_LEN 128
/* Constrainted by memhandle size */
#define NVSHMEMT_LIBFABRIC_MAX_NIC_PER_PE 16
#define NVSHMEMT_LIBFABRIC_PROXY_EP_IDX 1
#define NVSHMEMT_LIBFABRIC_QUIET_TIMEOUT_MS 20

/* Maximum size of inject data. Currently
 * the max size we will use is one element
 * of a given type. Making it 16 bytes in the
 * case of complex number support. */
#ifdef NVSHMEM_COMPLEX_SUPPORT
#define NVSHMEMT_LIBFABRIC_INJECT_BYTES 16
#else
#define NVSHMEMT_LIBFABRIC_INJECT_BYTES 8
#endif

#define NVSHMEMT_LIBFABRIC_MAX_RETRIES (1ULL << 20)

#ifndef container_of
#define container_of(ptr, type, field) ((type *)((char *)ptr - offsetof(type, field)))
#endif

typedef struct {
    char name[NVSHMEMT_LIBFABRIC_DOMAIN_LEN];
} nvshmemt_libfabric_domain_name_t;

typedef struct {
    char name[NVSHMEMT_LIBFABRIC_EP_LEN];
} nvshmemt_libfabric_ep_name_t;

struct nvshmemt_libfabric_gdr_op_ctx;
typedef struct nvshmemt_libfabric_gdr_op_ctx nvshmemt_libfabric_gdr_op_ctx_t;

#define NVSHMEM_STAGED_AMO_PUT_SIGNAL_SEQ_CNTR_BIT_SHIFT 28
#define NVSHMEM_STAGED_AMO_PUT_SIGNAL_SEQ_CNTR_BIT_MASK \
    ((1U << NVSHMEM_STAGED_AMO_PUT_SIGNAL_SEQ_CNTR_BIT_SHIFT) - 1)

/**
 * The last sequence number is reserved for atomic-only operations.
 * This will not be returned by the sequence counter.
 */
#define NVSHMEM_STAGED_AMO_SEQ_NUM NVSHMEM_STAGED_AMO_PUT_SIGNAL_SEQ_CNTR_BIT_MASK

/**
 * Type used for tracking sequence numbers for put-signal operations
 *
 * A sequence number is associated with each put-signal operation. This type
 * manages allocation of sequence numbers and return of sequence numbers via
 * acks from the remote side.
 */
struct nvshmemt_libfabric_endpoint_seq_counter_t {
    constexpr static uint32_t num_sequence_bits = NVSHMEM_STAGED_AMO_PUT_SIGNAL_SEQ_CNTR_BIT_SHIFT;

    /**
     * Sequence counter is composed of:
     *
     * |----------<num_sequence_bits>----------|
     * | <num_category_bits> <num_index_bits>  |
     */
    constexpr static uint32_t num_category_bits = 1;
    constexpr static uint32_t num_categories = (1U << num_category_bits);

    constexpr static uint32_t num_index_bits = (num_sequence_bits - num_category_bits);

    constexpr static uint32_t index_mask = ((1U << num_index_bits) - 1);
    constexpr static uint32_t category_mask = (1U << num_index_bits);

    constexpr static uint32_t sequence_mask = NVSHMEM_STAGED_AMO_PUT_SIGNAL_SEQ_CNTR_BIT_MASK;

    /**
     * The "category" is the bits before the index bit(s). Returns the category
     * bits shifted to the right.
     */
    constexpr static uint32_t get_category(uint32_t seq_num) {
        return ((seq_num & category_mask) >> num_index_bits);
    }

    /**
     * The "index" is the bits after the category bit(s)
     */
    constexpr static uint32_t get_index(uint32_t seq_num) { return seq_num & index_mask; }

    /* ------------------------------ */
    /* Member variables */

    uint32_t sequence_counter;
    uint32_t pending_acks[num_categories];
    uint32_t put_count;

    /**
     * Default constructor - initializes counter to zero
     */
    nvshmemt_libfabric_endpoint_seq_counter_t() {
        reset();
    }

    /**
     * Reset counter and pending acks to zero
     */
    void reset() {
        sequence_counter = 0;
        memset(pending_acks, 0, sizeof(pending_acks));
        put_count = 0;
    }

    /**
     * Obtain the next sequence number.
     *
     * @return -1 if no sequence number available
     */
    int32_t next_seq_num() {
        /* Skip this sequence number if reserved */
        if (sequence_counter == NVSHMEM_STAGED_AMO_SEQ_NUM) {
            sequence_counter = (sequence_counter + 1) & sequence_mask;
        }

        uint32_t seq_num = sequence_counter;

        uint32_t category = get_category(seq_num);

        /* For first sequence number of category, check if category is available */
        if (get_index(seq_num) == 0) {
            if (pending_acks[category] != 0) {
                /* No sequence number available */
                return -1;
            }
        }

        /* Can't have more outstanding acks than sequence numbers in the category */
        assert(pending_acks[category] <= index_mask);
        /* Increment pending acks */
        ++pending_acks[category];

        /* Increment sequence counter */
        sequence_counter = (sequence_counter + 1) & sequence_mask;
        return seq_num;
    }

    /**
     * Mark a previously issued seq_num as complete, decremeting the pending
     * acks counter for the category
     */
    void return_acked_seq_num(uint32_t seq_num) {
        assert(seq_num != NVSHMEM_STAGED_AMO_SEQ_NUM);

        uint32_t category = get_category(seq_num);

        assert(pending_acks[category] > 0);
        --pending_acks[category];
    }

    /**
     * Mark a range of sequence numbers as complete
     */
    void return_acked_seq_num_range(uint32_t start_seq, uint32_t end_seq) {
        assert(start_seq != NVSHMEM_STAGED_AMO_SEQ_NUM);
        assert(end_seq != NVSHMEM_STAGED_AMO_SEQ_NUM);

        uint32_t start_category = get_category(start_seq);
        uint32_t end_category = get_category(end_seq);

        uint32_t num_indexes;
        if (end_seq >= start_seq) {
            num_indexes = end_seq - start_seq + 1;
        } else {
            num_indexes = (sequence_mask - start_seq + 1) + (end_seq + 1);
        }

        if (start_category == end_category) {
            assert(pending_acks[start_category] >= num_indexes);
            pending_acks[start_category] -= num_indexes;
        } else {
            uint32_t count_in_start_cat = (index_mask + 1) - get_index(start_seq);
            uint32_t count_in_end_cat = get_index(end_seq) + 1;

            assert(pending_acks[start_category] >= count_in_start_cat);
            assert(pending_acks[end_category] >= count_in_end_cat);

            pending_acks[start_category] -= count_in_start_cat;
            pending_acks[end_category] -= count_in_end_cat;
        }
    }
};

typedef enum {
    NVSHMEMT_LIBFABRIC_SEND,
    NVSHMEMT_LIBFABRIC_ACK,
    NVSHMEMT_LIBFABRIC_MATCH,
    NVSHMEMT_LIBFABRIC_PUT_ACK_PASSTHROUGH,
} nvshmemt_libfabric_recv_t;

typedef enum {
    NVSHMEMT_LIBFABRIC_RECV_TYPE_ACK,
    NVSHMEMT_LIBFABRIC_RECV_TYPE_NOT_ACK,
} nvshmemt_libfabric_recv_type_t;

typedef struct {
    struct fid_ep *endpoint;
    struct fid_cq *cq;
    struct fid_cntr *counter;
    uint64_t submitted_ops;
    uint64_t completed_staged_atomics;
    int domain_index;
} nvshmemt_libfabric_endpoint_t;

// Entry types for completion map
enum nvshmemt_libfabric_comp_entry_type {
    NVSHMEMT_LIBFABRIC_COMP_ENTRY_SIGNAL,
    NVSHMEMT_LIBFABRIC_COMP_ENTRY_PUT_ACK
};

// Entry for signal operations (put-signal, atomic)
struct nvshmemt_libfabric_signal_comp_entry {
    nvshmemt_libfabric_gdr_op_ctx_t *op;
    int progress_count;
};

// Entry for puts that need acknowledgment
struct nvshmemt_libfabric_put_ack_entry {
    fi_addr_t src_addr;
    nvshmemt_libfabric_endpoint_t *ep;
};

// Tagged union for completion entries
struct nvshmemt_libfabric_comp_entry_t {
    nvshmemt_libfabric_comp_entry_type type;
    union {
        nvshmemt_libfabric_signal_comp_entry signal_entry;
        nvshmemt_libfabric_put_ack_entry ack_entry;
    };
};

typedef struct nvshmemt_libfabric_gdr_send_p_op {
    uint64_t value;
} nvshmemt_libfabric_gdr_send_p_op_t;

typedef struct nvshmemt_libfabric_gdr_send_amo_op {
    nvshmemi_amo_t op;
    void *target_addr;
    void *ret_addr;
    union {
        uint64_t retflag;
        uint32_t sequence_count;
    };
    uint64_t swap_add;
    uint64_t comp;
    uint32_t size;
    int src_pe;
} nvshmemt_libfabric_gdr_send_amo_op_t;

typedef struct nvshmemt_libfabric_gdr_ret_amo_op {
    void *ret_addr;
    g_elem_t elem;
} nvshmemt_libfabric_gdr_ret_amo_op_t;

struct nvshmemt_libfabric_gdr_op_ctx {
    nvshmemt_libfabric_recv_t type;
    nvshmemt_libfabric_endpoint_t *ep;
    union {
        nvshmemt_libfabric_gdr_send_p_op_t p_op;
        nvshmemt_libfabric_gdr_send_amo_op_t send_amo;
        nvshmemt_libfabric_gdr_ret_amo_op_t ret_amo;
    };
    struct fi_context2 ofi_context;
    fi_addr_t src_addr;
};

typedef enum {
    NVSHMEMT_LIBFABRIC_PROVIDER_VERBS = 0,
    NVSHMEMT_LIBFABRIC_PROVIDER_SLINGSHOT,
    NVSHMEMT_LIBFABRIC_PROVIDER_EFA
} nvshmemt_libfabric_provider;

typedef enum {
    NVSHMEMT_LIBFABRIC_CONTEXT_P_OP = 0,
    NVSHMEMT_LIBFABRIC_CONTEXT_SEND_AMO,
    NVSHMEMT_LIBFABRIC_CONTEXT_RECV_AMO
} nvshemmt_libfabric_context_t;

typedef enum {
    NVSHMEMT_LIBFABRIC_IMM_PUT_SIGNAL_SEQ = 0,
    NVSHMEMT_LIBFABRIC_IMM_STAGED_ATOMIC_ACK,
    NVSHMEMT_LIBFABRIC_IMM_STANDALONE_PUT,
    NVSHMEMT_LIBFABRIC_IMM_STANDALONE_PUT_WITH_ACK_REQ,
    NVSHMEMT_LIBFABRIC_IMM_STANDALONE_PUT_ACK,
    NVSHMEMT_LIBFABRIC_MSG_COALESCED_ACK,
} nvshmemt_libfabric_imm_cq_data_hdr_t;

/*
 * Conditional lock: skips locking when FI_THREAD_COMPLETION is active.
 *
 * With FI_PROGRESS_AUTO, we request FI_THREAD_COMPLETION from the provider,
 * meaning the host thread and proxy thread each operate on separate endpoints
 * (eps[0] vs eps[1+]), so their op_queues are disjoint and no synchronization
 * is needed. With FI_PROGRESS_MANUAL, we use FI_THREAD_SAFE because
 * manual_progress() iterates all EPs from both threads, requiring locking.
 */
class conditional_mutex {
    std::mutex mtx;
    bool needs_lock;

   public:
    conditional_mutex() : needs_lock(true) {}
    void set_needs_lock(bool v) { needs_lock = v; }
    void lock() { if (needs_lock) mtx.lock(); }
    void unlock() { if (needs_lock) mtx.unlock(); }
};

class threadSafeOpQueue {
   private:
    conditional_mutex send_mutex;
    conditional_mutex ack_recv_mutex;
    conditional_mutex other_recv_mutex;
    std::vector<void *> send;
    std::deque<void *> ack_recv;
    std::deque<void *> other_recv;

   public:
    threadSafeOpQueue() = default;
    threadSafeOpQueue(const threadSafeOpQueue &) = delete;
    threadSafeOpQueue &operator=(const threadSafeOpQueue &) = delete;

    /* Disable locking when FI_THREAD_COMPLETION keeps host/proxy EPs disjoint. */
    void set_auto_progress(bool auto_progress) {
        send_mutex.set_needs_lock(!auto_progress);
        ack_recv_mutex.set_needs_lock(!auto_progress);
        other_recv_mutex.set_needs_lock(!auto_progress);
    }

    int getNextSends(void **elems, size_t num_elems = 1) {
        send_mutex.lock();
        if (send.size() < num_elems) {
            for (size_t i = 0; i < num_elems; i++) {
                elems[i] = NULL;
            }
            send_mutex.unlock();
            return -EAGAIN;
        }
        for (size_t i = 0; i < num_elems; i++) {
            elems[i] = send.back();
            send.pop_back();
            assert(elems[i] != NULL);
        }
        send_mutex.unlock();
        return 0;
    }

    int getNextAmoOps(nvshmemt_libfabric_gdr_op_ctx_t *send_elems[2],
                      nvshmemt_libfabric_gdr_op_ctx_t **recv_elem,
                      nvshmemt_libfabric_recv_type_t recv_type) {
        int status = 0;
        int num_sends = 0;

        if (recv_type == NVSHMEMT_LIBFABRIC_RECV_TYPE_NOT_ACK) {
            other_recv_mutex.lock();
            if (other_recv.empty()) {
                *recv_elem = NULL;
                other_recv_mutex.unlock();
                return 0;
            }
            *recv_elem = (nvshmemt_libfabric_gdr_op_ctx_t *)other_recv.front();
            if ((&((*recv_elem)->send_amo))->op > NVSHMEMI_AMO_END_OF_NONFETCH) {
                num_sends = 2;
            } else {
                num_sends = 1;
            }
            status = getNextSends((void **)send_elems, num_sends);
            if (status == -EAGAIN) {
                *recv_elem = NULL;
                other_recv_mutex.unlock();
                return -EAGAIN;
            }
            assert(recv_elem != NULL);
            for (int i = 0; i < num_sends; i++) {
                assert(send_elems[i] != NULL);
            }
            other_recv.pop_front();
            other_recv_mutex.unlock();
            return 0;
        } else if (recv_type == NVSHMEMT_LIBFABRIC_RECV_TYPE_ACK) {
            ack_recv_mutex.lock();
            if (ack_recv.empty()) {
                *recv_elem = NULL;
                ack_recv_mutex.unlock();
                return 0;
            }
            *recv_elem = (nvshmemt_libfabric_gdr_op_ctx_t *)ack_recv.front();
            assert(*recv_elem != NULL);
            ack_recv.pop_front();
            ack_recv_mutex.unlock();
            return 0;
        } else {
            fprintf(stderr, "getNextAmoOps: invalid recv_type: %d\n", recv_type);
            assert(false);
            return -EINVAL;
        }
    }

    void putToSend(void *elem) {
        send_mutex.lock();
        send.push_back(elem);
        send_mutex.unlock();
        return;
    }

    void putToSendBulk(char *elem, size_t elem_size, size_t num_elems) {
        send_mutex.lock();
        for (size_t i = 0; i < num_elems; i++) {
            send.push_back(elem);
            elem = elem + elem_size;
        }
        send_mutex.unlock();
        return;
    }

    void *getNextRecv(nvshmemt_libfabric_recv_type_t recv_type) {
        void *elem = NULL;
        if (recv_type == NVSHMEMT_LIBFABRIC_RECV_TYPE_ACK) {
            ack_recv_mutex.lock();
            if (ack_recv.empty()) {
                ack_recv_mutex.unlock();
                return NULL;
            }

            elem = ack_recv.front();
            ack_recv.pop_front();
            ack_recv_mutex.unlock();
            return elem;
        } else {
            other_recv_mutex.lock();
            if (other_recv.empty()) {
                other_recv_mutex.unlock();
                return NULL;
            }
            elem = other_recv.front();
            other_recv.pop_front();
            other_recv_mutex.unlock();
            return elem;
        }
    }

    void putToRecv(void *elem, nvshmemt_libfabric_recv_type_t recv_type) {
        if (recv_type == NVSHMEMT_LIBFABRIC_RECV_TYPE_ACK) {
            ack_recv_mutex.lock();
            ack_recv.push_back(elem);
            ack_recv_mutex.unlock();
        } else if (recv_type == NVSHMEMT_LIBFABRIC_RECV_TYPE_NOT_ACK) {
            other_recv_mutex.lock();
            other_recv.push_back(elem);
            other_recv_mutex.unlock();
        } else {
            fprintf(stderr, "putToRecv: invalid recv_type: %d\n", recv_type);
            assert(false);
            return;
        }
    }
};

typedef struct {
    std::unordered_map<int, nvshmemt_libfabric_endpoint_seq_counter_t> *put_signal_seq_counter_per_pe;
    std::unordered_map<uint64_t, nvshmemt_libfabric_comp_entry_t> *proxy_put_signal_comp_map;
    std::unordered_map<int, uint32_t> *next_expected_seq;
    struct nvshmemt_libfabric_ack_aggregator_t *ack_aggregator;
} nvshmemt_libfabric_signal_state_t;

struct signal_delivery_work_entry {
    nvshmemt_libfabric_gdr_op_ctx_t *op;
    nvshmemt_libfabric_gdr_op_ctx_t *send_elems[2];
    uint32_t sequence_count;
};

struct signal_delivery_done_entry {
    nvshmemt_libfabric_gdr_op_ctx_t *op;
    nvshmemt_libfabric_gdr_op_ctx_t *send_elems[2];
    uint32_t sequence_count;
    int src_pe;
    fi_addr_t src_addr;
    nvshmemt_libfabric_endpoint_t *ep;
    bool is_fetch_amo;
    uint64_t old_value;
    uint64_t ret_flags;
    void *ret_addr;
};

template <typename T, int CAPACITY = 1024>
class SPSCRing {
    T ring[CAPACITY];
    alignas(64) std::atomic<int> head{0};
    alignas(64) std::atomic<int> tail{0};
   public:
    bool push(const T &entry) {
        int h = head.load(std::memory_order_relaxed);
        int next = (h + 1) % CAPACITY;
        if (next == tail.load(std::memory_order_acquire)) return false;
        ring[h] = entry;
        head.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T &entry) {
        int t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        entry = ring[t];
        tail.store((t + 1) % CAPACITY, std::memory_order_release);
        return true;
    }
};

typedef struct {
    struct fi_info *all_prov_info;
    std::vector<struct fi_info *> prov_infos;
    std::vector<struct fid_fabric *> fabrics;
    std::vector<struct fid_domain *> domains;
    std::vector<struct fid_av *> addresses;
    std::vector<nvshmemt_libfabric_endpoint_t *> eps;

    nvshmemt_libfabric_domain_name_t *domain_names;
    int num_domains;
    nvshmemt_libfabric_provider provider;
    int log_level;
    struct nvshmemi_cuda_fn_table *table;
    struct transport_mem_handle_info_cache *cache;

    /* Required for multi-rail */
    int max_nic_per_pe;
    int num_selected_devs;
    int num_selected_domains;
    int cur_proxy_ep_index;

    /* Required for staged_amo */
    std::vector<threadSafeOpQueue *> op_queue;
    std::vector<struct fid_mr *> mr;
    std::vector<void *> send_buf;
    std::vector<void *> recv_buf;
    std::vector<struct fid_mr *> mr_staged_amo_ack;
    void **remote_addr_staged_amo_ack;
    uint64_t *rkey_staged_amo_ack;

    /* Signal ordering state */
    nvshmemt_libfabric_signal_state_t host_signal_state;
    nvshmemt_libfabric_signal_state_t proxy_signal_state;

    /* Max ops per progress iteration */
    int proxy_request_batch_max;

    /* Signal delivery thread (v2: queue-based, no libfabric sharing) */
    pthread_t signal_delivery_thread;
    std::atomic<int> signal_delivery_stop{0};
    void *signal_delivery_transport;
    int spawner_cpu;
    std::atomic_flag signal_queue_lock = ATOMIC_FLAG_INIT;
    SPSCRing<signal_delivery_work_entry> signal_work_queue;
    SPSCRing<signal_delivery_done_entry> signal_done_queue;

    /* Put ack passthrough pool: pre-allocated op_ctx entries for routing
     * standalone put acks through the signal delivery pipeline */
    std::vector<nvshmemt_libfabric_gdr_op_ctx_t *> put_ack_passthrough_pool;
} nvshmemt_libfabric_state_t;

typedef struct {
    struct fid_mr *mr;
    uint64_t key;
    void *local_desc;
} nvshmemt_libfabric_mem_handle_ep_t;

typedef struct {
    size_t gdr_mapping_size;
    void *ptr;
    void *cpu_ptr;
#ifdef NVSHMEM_USE_GDRCOPY
    gdr_mh_t mh;
    void *cpu_ptr_base;
#endif
} nvshmemt_libfabric_memhandle_info_t;

typedef struct {
    void *buf;
    nvshmemt_libfabric_mem_handle_ep_t hdls[1 + NVSHMEMT_LIBFABRIC_MAX_NIC_PER_PE];
} nvshmemt_libfabric_mem_handle_t;

/* Wire data for put-signal gdr staged atomics
 * 32 bytes
 * | 4 type | 2 op | 2 num_writes | 8 signal | 8 target_addr | 4 sequence_count | 4 resv
 */
typedef struct nvshmemt_libfabric_gdr_signal_op {
    nvshmemt_libfabric_recv_t type; /* Must be first */
    uint16_t op;
    uint16_t num_writes;
    uint64_t sig_val;
    void *target_addr;
    uint32_t sequence_count;
    uint32_t src_pe;
} nvshmemt_libfabric_gdr_signal_op_t;
/*  EFA's inline send size is 32 bytes */
static_assert(sizeof(nvshmemt_libfabric_gdr_signal_op_t) == 32);

/* Wire format for coalesced ack message, sent via fi_send inline.
 * 24 bytes
 * | 4 header | 4 src_pe | 4 range_start | 4 range_count | 4 amo_ack_count | 4 reserved |
 */
struct nvshmemt_libfabric_coalesced_ack_t {
    uint32_t header;        /* NVSHMEMT_LIBFABRIC_MSG_COALESCED_ACK */
    uint32_t src_pe;        /* Source peer ID (the receiver sending the ack) */
    uint32_t range_start;   /* Start of put/signal sequence number range */
    uint32_t range_count;   /* Number of contiguous sequence numbers in range */
    uint32_t amo_ack_count; /* Number of AMO acks (dummy seq num operations) */
    uint32_t reserved;      /* Padding / future use */
};
static_assert(sizeof(nvshmemt_libfabric_coalesced_ack_t) <= 32);

struct nvshmemt_libfabric_peer_pending_acks_t {
    /* Contiguous range tracking for put/signal acks */
    uint32_t range_start;   /* First sequence number in pending range */
    uint32_t range_count;   /* Number of contiguous sequence numbers */
    bool has_range;          /* Whether a range is active */

    /* AMO ack count (operations using NVSHMEM_STAGED_AMO_SEQ_NUM) */
    uint32_t amo_ack_count;

    /* Total pending acks (range_count + amo_ack_count) for threshold check */
    uint32_t total_pending() const { return range_count + amo_ack_count; }
};

struct nvshmemt_libfabric_ack_aggregator_t {
    std::unordered_map<int, nvshmemt_libfabric_peer_pending_acks_t> pending_per_peer;
    std::vector<int> dirty_peers; /* PEs with non-zero pending acks */
    uint32_t flush_threshold;     /* Default: 16 */

    void record_ack(int pe, uint32_t seq_num, nvshmem_transport_t transport,
                    nvshmemt_libfabric_endpoint_t *ep, fi_addr_t dest_addr);
    void record_amo_ack(int pe, nvshmem_transport_t transport,
                        nvshmemt_libfabric_endpoint_t *ep, fi_addr_t dest_addr);
    int flush_peer(int pe, nvshmem_transport_t transport,
                   nvshmemt_libfabric_endpoint_t *ep, fi_addr_t dest_addr);
    int flush_all(nvshmem_transport_t transport,
                  nvshmemt_libfabric_endpoint_t *ep);
};
