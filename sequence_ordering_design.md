# Sequence Ordering for Atomic Operations

## Problem

Currently, atomic operations are added to the queue in `nvshmemt_libfabric_put_signal_completion` (line 621) in arbitrary order. Operations can complete out-of-order due to network conditions, but they must be processed in order of increasing sequence number to maintain correctness.

Additionally, sequence numbers must be allocated per-destination-peer to ensure that each receiver sees consecutive sequence numbers from each sender.

## Solution

Reuse the existing `proxy_put_signal_comp_map` to buffer operations until they can be drained in sequence order. Operations remain in the map (even when ready) until all prior sequence numbers have been processed.

Change sequence number allocation from per-endpoint to per-destination-peer to ensure consecutive sequence numbers for each sender-receiver pair.

## Data Structure

Add to `nvshmemt_libfabric_endpoint_t`:

```cpp
std::unordered_map<fi_addr_t, uint32_t> *next_expected_seq;
std::unordered_map<fi_addr_t, nvshmemt_libfabric_endpoint_seq_counter_t> *put_signal_seq_counter_per_pe;
```

- `next_expected_seq`: Pointer to map from each source address to the next sequence number expected from that source (initialized to 0)
- `put_signal_seq_counter_per_pe`: Pointer to map from destination `fi_addr_t` to sequence counter for that destination
- Both allocated with `new` during endpoint initialization, deallocated with `delete` during cleanup
- Reuse existing `proxy_put_signal_comp_map`: Already stores operations by `(addr << 32 | seq_num)` with readiness tracked by `progress_count`

## Algorithm

In `nvshmemt_libfabric_put_signal_completion`, replace the immediate queue insertion:

```cpp
if (!iter->second.second) {
    if (is_write_comp) {
        op = iter->second.first;
    }

    nvshmemtLibfabricOpQueue.putToRecv(op, NVSHMEMT_LIBFABRIC_RECV_TYPE_NOT_ACK);
    ep->proxy_put_signal_comp_map->erase(iter);
}
```

With ordered draining logic:

```cpp
if (!iter->second.second) {
    if (is_write_comp) {
        op = iter->second.first;
    }
    
    // This operation is now ready - try to drain in sequence order
    fi_addr_t src_addr = *addr;
    uint32_t &next_seq = ep->next_expected_seq[src_addr];
    
    while (true) {
        // Skip reserved sequence number
        if (next_seq == NVSHMEM_STAGED_AMO_SEQ_NUM) {
            next_seq = (next_seq + 1) & nvshmemt_libfabric_endpoint_seq_counter_t::sequence_mask;
            continue;
        }
        
        uint64_t key = (uint64_t)src_addr << 32 | next_seq;
        auto it = ep->proxy_put_signal_comp_map->find(key);
        
        // Stop if: operation doesn't exist OR operation not ready (progress_count != 0)
        if (it == ep->proxy_put_signal_comp_map->end() || it->second.second != 0) break;
        
        nvshmemtLibfabricOpQueue.putToRecv(it->second.first, NVSHMEMT_LIBFABRIC_RECV_TYPE_NOT_ACK);
        ep->proxy_put_signal_comp_map->erase(it);
        next_seq = (next_seq + 1) & nvshmemt_libfabric_endpoint_seq_counter_t::sequence_mask;
    }
}
// Note: removed the erase at the end - operations stay in map until drained in order
```

## Behavior

1. When an operation's `progress_count` reaches 0, it becomes ready but stays in `proxy_put_signal_comp_map`
2. Check if the next expected sequence number is ready (exists in map with `progress_count == 0`)
3. If yes, drain it and all consecutive ready operations to the queue
4. If no, leave ready operations in the map until earlier sequence numbers arrive and are processed

## Example

Sequence numbers arrive: 2, 0, 3, 1

- Receive seq 2: Buffer it (waiting for 0)
- Receive seq 0: Drain 0, then 1 is missing, stop
- Receive seq 3: Buffer it (waiting for 1)
- Receive seq 1: Drain 1, 2, 3 consecutively

## Considerations

- Sequence numbers wrap at uint32_t boundary (handled naturally by modular arithmetic)
- `NVSHMEM_STAGED_AMO_SEQ_NUM` is reserved for atomic-only operations and must be skipped during draining
- Memory overhead: Reuses existing map, adds per-PE sequence counters
- Operations stay in `proxy_put_signal_comp_map` longer (until sequence order allows draining)
- No bound on map size (may need monitoring/limits in production)
- Sequence counters are per-destination-peer to ensure consecutive sequence numbers for each sender-receiver pair
