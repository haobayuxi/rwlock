// // Author: Ming Zhang
// // Copyright (c) 2022

// #pragma once

#include "util/debug.h"

// #include "allocator/region_allocator.h"
#include "base/common.h"
// #include "cache/lock_status.h"
// #include "cache/version_status.h"
#include "connection/qp_manager.h"
// struct thread_params {
//   t_id_t thread_local_id;
//   t_id_t thread_global_id;
//   t_id_t thread_num_per_machine;
//   t_id_t total_thread_num;
//   MetaManager* global_meta_man;
//   // VersionCache* global_status;
//   // LockCache* global_lcache;
//   RDMARegionAllocator* global_rdma_region;
//   int coro_num;
//   std::string bench_name;
// };

// void run_thread(thread_params* params,
//                 TATP* tatp_client,
//                 SmallBank* smallbank_client,
//                 TPCC* tpcc_client);