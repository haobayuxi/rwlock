// Author: Ming Zhang
// Copyright (c) 2022

#pragma once

#include "memstore/hash_store.h"
#include "rlib/rdma_ctrl.hpp"

enum DTX_SYS : int {
  RWLock = 0,
  DrTMH = 1,
  DLMR = 2,
  OCC = 3,
};

enum OP : int {
  Read = 0,
  Write = 1,
  Insert = 2,
};

enum TXStatus : int {
  TX_INIT = 0,  // Transaction initialization
  TX_EXE,       // Transaction execution, read only
  TX_LOCK,      // Transaction execution, read+lock
  TX_VAL,       // Transaction validate
  TX_COMMIT,    // Commit primary and backups
  TX_ABORT      // Aborted transaction
};

enum ValStatus : int {
  RDMA_ERROR = -1,  // Validation network error
  NO_NEED_VAL = 0,  // Do not need validation, i.e., the coroutine does not need
                    // to yield CPU
  NEED_VAL = 1,     // Need validation, i.e., the coroutine needs to yield CPU
  MUST_ABORT =
      2  // The data version must be changed and hence no validation is needed
};

// Following are stuctures for maintaining coroutine's state, similar to context
// switch

struct DataSetItem {
  DataItemPtr item_ptr;
  bool is_fetched;
  bool is_logged;
  node_id_t read_which_node;  // From which node this data item is read. This is
                              // a node id, e.g., 0, 1, 2...
};

struct OldVersionForInsert {
  table_id_t table_id;
  itemkey_t key;
  version_t version;
};

struct LockAddr {
  node_id_t node_id;
  uint64_t lock_addr;
};

// For coroutines
struct DirectRead {
  RCQP* qp;
  DataSetItem* item;
  char* buf;
  node_id_t remote_node;
};

struct HashRead {
  RCQP* qp;
  DataSetItem* item;
  char* buf;
  node_id_t remote_node;
  const HashMeta meta;
  OP op;
};

struct InvisibleRead {
  RCQP* qp;
  char* buf;
  uint64_t off;
};

struct CasRead {
  RCQP* qp;
  DataSetItem* item;
  char* cas_buf;
  char* data_buf;
  node_id_t primary_node_id;
  OP op;
};

struct InsertOffRead {
  RCQP* qp;
  DataSetItem* item;
  char* buf;
  node_id_t remote_node;
  const HashMeta meta;
  offset_t node_off;
};

struct ValidateRead {
  RCQP* qp;
  DataSetItem* item;
  char* cas_buf;
  char* version_buf;
  bool has_lock_in_validate;
};

struct Lock {
  RCQP* qp;
  DataSetItem* item;
  char* cas_buf;
  uint64_t lock_off;
};

struct Unlock {
  char* cas_buf;
};

struct Version {
  DataSetItem* item;
  char* version_buf;
};

struct CommitWrite {
  node_id_t node_id;
  uint64_t lock_off;
};
