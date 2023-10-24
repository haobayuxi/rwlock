#include "dtx.h"

bool DTX::OOCC(coro_yield_t& yield) {
  bool read_only = read_write_set.empty();
  OccReadOnly(yield);
  if (!read_only) {
    CasWriteLockAndRead(yield);
  }
  if (start_time == 0) {
    start_time = get_clock_sys_time_us();
  }
  coro_sched->Yield(yield, coro_id);
  if (!read_only) {
    auto _wlock_start_time = get_clock_sys_time_us();
    if (wlock_start_time < _wlock_start_time) {
      wlock_start_time = _wlock_start_time;
    }
  }

  // Receive data
  return OOCCCheck(yield, read_only);
}

bool DTX::OccReadOnly(coro_yield_t& yield) {
  for (auto& item : read_only_set) {
    if (item.is_fetched) continue;
    auto it = item.item_ptr;
    node_id_t remote_node_id = 0;
    RCQP* qp = thread_qp_man->GetRemoteDataQPWithNodeID(remote_node_id);
    item.read_which_node = remote_node_id;
    auto offset = addr_cache->Search(remote_node_id, it->table_id, it->key);
    // auto offset = it->key;
    if (offset != NOT_FOUND) {
      it->remote_offset = offset;
      char* data_buf = thread_rdma_buffer_alloc->Alloc(DataItemSize);
      pending_direct_ro.emplace_back(DirectRead{.qp = qp,
                                                .item = &item,
                                                .buf = data_buf,
                                                .remote_node = remote_node_id});
      if (unlikely(!coro_sched->RDMARead(coro_id, qp, data_buf, offset,
                                         DataItemSize))) {
        return false;
      }
    } else {
      // hash read
      // RDMA_LOG(INFO) << "hash read";
      HashMeta meta =
          global_meta_man->GetPrimaryHashMetaWithTableID(it->table_id);
      uint64_t idx = MurmurHash64A(it->key, 0xdeadbeef) % meta.bucket_num;
      offset_t node_off = idx * meta.node_size + meta.base_off;
      char* local_hash_node = thread_rdma_buffer_alloc->Alloc(sizeof(HashNode));
      pending_hash_ro.emplace_back(HashRead{.qp = qp,
                                            .item = &item,
                                            .buf = local_hash_node,
                                            .remote_node = remote_node_id,
                                            .meta = meta});
      if (!coro_sched->RDMARead(coro_id, qp, local_hash_node, node_off,
                                sizeof(HashNode))) {
        return false;
      }
    }
  }
  return true;
}

bool DTX::CasWriteLockAndRead(coro_yield_t& yield) {
  // For read-write set, we need to read and lock them
  for (size_t i = 0; i < read_write_set.size(); i++) {
    if (read_write_set[i].is_fetched) continue;
    auto it = read_write_set[i].item_ptr;
    auto remote_node_id = global_meta_man->GetPrimaryNodeID(it->table_id);
    read_write_set[i].read_which_node = remote_node_id;
    RCQP* qp = thread_qp_man->GetRemoteDataQPWithNodeID(remote_node_id);
    auto offset = addr_cache->Search(remote_node_id, it->table_id, it->key);
    // Addr cached in local
    if (offset != NOT_FOUND) {
      // hit_local_cache_times++;
      it->remote_offset = offset;
      locked_rw_set.emplace_back(i);
      // After getting address, use doorbell CAS + READ
      char* cas_buf = thread_rdma_buffer_alloc->Alloc(sizeof(lock_t));
      char* data_buf = thread_rdma_buffer_alloc->Alloc(DataItemSize);
      pending_cas_rw.emplace_back(CasRead{.qp = qp,
                                          .item = &read_write_set[i],
                                          .cas_buf = cas_buf,
                                          .data_buf = data_buf,
                                          .primary_node_id = remote_node_id});
      if (!coro_sched->RDMACAS(coro_id, qp, cas_buf,
                               it->GetRemoteLockAddr(offset), 0, tx_id)) {
        return false;
      }
      if (!coro_sched->RDMARead(coro_id, qp, data_buf, offset, DataItemSize)) {
        return false;
      }
    } else {
      // Only read
      const HashMeta& meta =
          global_meta_man->GetPrimaryHashMetaWithTableID(it->table_id);
      uint64_t idx = MurmurHash64A(it->key, 0xdeadbeef) % meta.bucket_num;
      offset_t node_off = idx * meta.node_size + meta.base_off;
      char* local_hash_node = thread_rdma_buffer_alloc->Alloc(sizeof(HashNode));
      //   if (it->user_insert) {
      //     pending_insert_off_rw.emplace_back(
      //         InsertOffRead{.qp = qp,
      //                       .item = &read_write_set[i],
      //                       .buf = local_hash_node,
      //                       .remote_node = remote_node_id,
      //                       .meta = meta,
      //                       .node_off = node_off});
      //   } else {
      pending_hash_rw.emplace_back(HashRead{.qp = qp,
                                            .item = &read_write_set[i],
                                            .buf = local_hash_node,
                                            .remote_node = remote_node_id,
                                            .meta = meta});
      //   }
      if (!coro_sched->RDMARead(coro_id, qp, local_hash_node, node_off,
                                sizeof(HashNode))) {
        return false;
      }
    }
  }
  return true;
}

bool DTX::Validate(coro_yield_t& yield) {
  // The transaction is read-write, and all the written data have
  // been locked before
  // validate the reads
  std::vector<ValidateRead> pending_validate;

  // For read-only items, we only need to read their versions
  for (auto& set_it : read_only_set) {
    auto it = set_it.item_ptr;
    // RDMA_LOG(INFO) << "validate key = " << it->key;
    RCQP* qp = thread_qp_man->GetRemoteDataQPWithNodeID(set_it.read_which_node);
    char* version_buf = thread_rdma_buffer_alloc->Alloc(sizeof(version_t));
    pending_validate.push_back(ValidateRead{.qp = qp,
                                            .item = &set_it,
                                            .cas_buf = nullptr,
                                            .version_buf = version_buf,
                                            .has_lock_in_validate = false});
    if (!coro_sched->RDMARead(coro_id, qp, version_buf,
                              it->GetRemoteVersionAddr(), sizeof(version_t))) {
      return false;
    }
  }
  // Yield to other coroutines when waiting for network replies
  coro_sched->Yield(yield, coro_id);

  auto res = CheckValidate(pending_validate);
  return res;
}

bool DTX::CheckValidate(std::vector<ValidateRead>& pending_validate) {
  for (auto& re : pending_validate) {
    auto it = re.item->item_ptr;
    // Compare version
    if (it->version != *((version_t*)re.version_buf)) {
      return false;
    }
  }
  return true;
}

void DTX::ParallelUndoLog() {
  // Write the old data from read write set
  size_t log_size = sizeof(tx_id) + sizeof(t_id);
  for (auto& set_it : read_write_set) {
    if (!set_it.is_logged && !set_it.item_ptr->user_insert) {
      // For the newly inserted data, the old data are not needed to be
      // recorded
      log_size += DataItemSize;
    }
  }
  char* written_log_buf = thread_rdma_buffer_alloc->Alloc(log_size);

  offset_t cur = 0;
  *((tx_id_t*)(written_log_buf + cur)) = tx_id;
  cur += sizeof(tx_id);
  *((t_id_t*)(written_log_buf + cur)) = t_id;
  cur += sizeof(t_id);

  for (auto& set_it : read_write_set) {
    if (!set_it.is_logged && !set_it.item_ptr->user_insert) {
      memcpy(written_log_buf + cur, (char*)(set_it.item_ptr.get()),
             DataItemSize);
      cur += DataItemSize;
      set_it.is_logged = true;
    }
  }

  offset_t log_offset =
      thread_remote_log_offset_alloc->GetNextLogOffset(0, log_size);
  RCQP* qp = thread_qp_man->GetRemoteLogQPWithNodeID(0);
  coro_sched->RDMALog(coro_id, tx_id, qp, written_log_buf, log_offset,
                      log_size);
}

bool DTX::CheckDirectRO(std::vector<DirectRead>& pending_direct_ro) {
  // check if the tuple has been wlocked
  for (auto& res : pending_direct_ro) {
    // auto* it = res.item->item_ptr.get();
    res.item->is_fetched = true;
    auto* fetched_item = (DataItem*)res.buf;
    if (fetched_item->lock == W_LOCKED) return false;
  }

  return true;
}

bool DTX::CheckHashRO(std::vector<HashRead>& pending_hash_ro,
                      std::list<HashRead>& pending_next_hash_ro) {
  // Check results from hash read
  for (auto& res : pending_hash_ro) {
    auto* local_hash_node = (HashNode*)res.buf;
    auto* it = res.item->item_ptr.get();
    bool find = false;

    for (auto& item : local_hash_node->data_items) {
      if (item.valid && item.key == it->key && item.table_id == it->table_id) {
        *it = item;
        addr_cache->Insert(res.remote_node, it->table_id, it->key,
                           it->remote_offset);
        res.item->is_fetched = true;
        find = true;
        break;
      }
    }
    if (likely(find)) {
      if (unlikely(it->lock == W_LOCKED)) {
        return false;
      } else {
        if (local_hash_node->next == nullptr) return false;
        // Not found, we need to re-read the next bucket
        auto node_off = (uint64_t)local_hash_node->next - res.meta.data_ptr +
                        res.meta.base_off;
        pending_next_hash_ro.emplace_back(
            HashRead{.qp = res.qp,
                     .item = res.item,
                     .buf = res.buf,
                     .remote_node = res.remote_node,
                     .meta = res.meta});
        if (!coro_sched->RDMARead(coro_id, res.qp, res.buf, node_off,
                                  sizeof(HashNode)))
          return false;
      }
    }
  }
  return true;
}

bool DTX::CheckNextHashRO(std::list<HashRead>& pending_next_hash_ro) {
  for (auto iter = pending_next_hash_ro.begin();
       iter != pending_next_hash_ro.end(); iter++) {
    auto res = *iter;
    auto* local_hash_node = (HashNode*)res.buf;
    auto* it = res.item->item_ptr.get();
    bool find = false;

    for (auto& item : local_hash_node->data_items) {
      if (item.valid && item.key == it->key && item.table_id == it->table_id) {
        *it = item;
        addr_cache->Insert(res.remote_node, it->table_id, it->key,
                           it->remote_offset);
        res.item->is_fetched = true;
        find = true;
        break;
      }
    }
    if (likely(find)) {
      if (unlikely(it->lock == W_LOCKED)) {
        return false;
      } else {
        if (local_hash_node->next == nullptr) return false;
        // Not found, we need to re-read the next bucket
        auto node_off = (uint64_t)local_hash_node->next - res.meta.data_ptr +
                        res.meta.base_off;
        pending_next_hash_ro.emplace_back(
            HashRead{.qp = res.qp,
                     .item = res.item,
                     .buf = res.buf,
                     .remote_node = res.remote_node,
                     .meta = res.meta});
        if (!coro_sched->RDMARead(coro_id, res.qp, res.buf, node_off,
                                  sizeof(HashNode)))
          return false;
      }
    }
  }
  return true;
}

bool DTX::OOCCCheck(coro_yield_t& yield, bool read_only) {
  if (!CheckDirectRO(pending_direct_ro)) return false;
  if (!CheckHashRO(pending_hash_ro, pending_next_hash_ro)) return false;
  if (!read_only) {
    // check rw results
  }

  // During results checking, we may re-read data due to invisibility and hash
  // collisions
  while (unlikely(!pending_next_hash_ro.empty())) {
    coro_sched->Yield(yield, coro_id);
    if (!CheckNextHashRO(pending_next_hash_ro)) return false;
  }

  return true;
}

bool DTX::CheckCasRw() {
  // check if w locked
  for (auto& res : pending_cas_ro) {
    // auto* it = res.item->item_ptr.get();
    res.item->is_fetched = true;
    auto cas = (uint64_t)*res.cas_buf;
    if (cas != tx_id) return false;
  }
  return true;
}

bool DTX::CheckHashRw() { return true; }