// Copyright (c) 2015, Robert Escriva
// All rights reserved.

#ifndef consus_txman_transaction_h_
#define consus_txman_transaction_h_

// consus
#include <consus.h>
#include "namespace.h"
#include "common/consus.h"
#include "common/ids.h"
#include "common/transaction_id.h"
#include "common/transaction_group.h"
#include "txman/log_entry_t.h"
#include "txman/paxos_synod.h"

BEGIN_CONSUS_NAMESPACE
class daemon;

class transaction
{
    public:
        enum state_t
        {
            INITIALIZED,
            EXECUTING,
            LOCAL_COMMIT_VOTE,
            GLOBAL_COMMIT_VOTE,
            COMMITTED,
            ABORTED,
            TERMINATED,
            COLLECTED
        };

    public:
        transaction(const transaction_group& tg);
        ~transaction() throw ();

    public:
        const transaction_group& state_key() const;
        bool finished();

    // commands coming from the client
    public:
        void begin(comm_id id, uint64_t nonce, uint64_t timestamp,
                   const paxos_group& group,
                   const std::vector<paxos_group_id>& dcs,
                   daemon* d);
        void read(comm_id id, uint64_t nonce, uint64_t seqno,
                  const e::slice& table,
                  const e::slice& key,
                  std::auto_ptr<e::buffer> backing,
                  daemon* d);
        void write(comm_id id, uint64_t nonce, uint64_t seqno,
                   const e::slice& table,
                   const e::slice& key,
                   const e::slice& value,
                   std::auto_ptr<e::buffer> backing,
                   daemon* d);
        void prepare(comm_id id, uint64_t nonce, uint64_t seqno, daemon* d);
        void abort(comm_id id, uint64_t nonce, uint64_t seqno, daemon* d);

    public:
        void paxos_2a(uint64_t seqno, log_entry_t t, e::unpacker up,
                      std::auto_ptr<e::buffer> backing, daemon* d);
        void paxos_2b(comm_id id, uint64_t seqno, daemon* d);
        void commit_record(e::slice commit_record,
                           std::auto_ptr<e::buffer> _backing,
                           daemon* d);
        void callback_durable(uint64_t seqno, daemon* d);

        // key value store callbacks
        void callback_locked(consus_returncode rc, uint64_t seqno, daemon* d);
        void callback_unlocked(consus_returncode rc, uint64_t seqno, daemon* d);
        void callback_read(consus_returncode rc, uint64_t timestamp, const e::slice& value,
                           uint64_t seqno, daemon*d);
        void callback_write(consus_returncode rc, uint64_t seqno, daemon* d);
        void callback_verify_read(consus_returncode rc, uint64_t timestamp, const e::slice& value,
                                  uint64_t seqno, daemon*d);
        void callback_verify_write(consus_returncode rc, uint64_t timestamp, const e::slice& value,
                                   uint64_t seqno, daemon*d);

        void externally_work_state_machine(daemon* d);

    private:
        struct operation;
        struct comparison;

    private:
        std::string logid();
        void ensure_initialized();
        void paxos_2a_begin(uint64_t seqno, e::unpacker up,
                            std::auto_ptr<e::buffer> backing, daemon* d);
        void paxos_2a_read(uint64_t seqno, e::unpacker up,
                           std::auto_ptr<e::buffer> backing, daemon* d);
        void paxos_2a_write(uint64_t seqno, e::unpacker up,
                            std::auto_ptr<e::buffer> backing, daemon* d);
        void paxos_2a_prepare(uint64_t seqno, e::unpacker up,
                              std::auto_ptr<e::buffer> backing, daemon* d);
        void paxos_2a_abort(uint64_t seqno, e::unpacker up,
                            std::auto_ptr<e::buffer> backing, daemon* d);
        void commit_record_begin(uint64_t seqno, e::unpacker up,
                                 e::compat::shared_ptr<e::buffer> backing, daemon* d);
        void commit_record_read(uint64_t seqno, e::unpacker up,
                                e::compat::shared_ptr<e::buffer> backing, daemon* d);
        void commit_record_write(uint64_t seqno, e::unpacker up,
                                 e::compat::shared_ptr<e::buffer> backing, daemon* d);
        void commit_record_prepare(uint64_t seqno, e::unpacker up,
                                   e::compat::shared_ptr<e::buffer> backing, daemon* d);
        void internal_begin(const char* source, uint64_t timestamp,
                            const paxos_group& group,
                            const std::vector<paxos_group_id>& dcs,
                            daemon* d);
        void internal_read(const char* source, uint64_t seqno,
                           const e::slice& table,
                           const e::slice& key,
                           e::compat::shared_ptr<e::buffer> backing,
                           daemon* d);
        void internal_write(const char* source, uint64_t seqno,
                            const e::slice& table,
                            const e::slice& key,
                            const e::slice& value,
                            e::compat::shared_ptr<e::buffer> backing,
                            daemon* d);
        void internal_end_of_transaction(const char* source,
                                         const char* op,
                                         log_entry_t let,
                                         uint64_t seqno,
                                         daemon* d);
        void internal_paxos_2b(comm_id id, uint64_t seqno, daemon* d);

        void work_state_machine(daemon* d);
        void work_state_machine_executing(daemon* d);
        void work_state_machine_local_commit_vote(daemon* d);
        void work_state_machine_global_commit_vote(daemon* d);
        void work_state_machine_committed(daemon* d);
        void work_state_machine_aborted(daemon* d);

        // execution utils
        void avoid_commit_if_possible(daemon* d);
        bool is_durable(uint64_t seqno);
        bool resize_to_hold(uint64_t seqno);

        // key value store utils
        void acquire_lock(uint64_t seqno, daemon* d);
        void release_lock(uint64_t seqno, daemon* d);
        void start_read(uint64_t seqno, daemon* d);
        void start_write(uint64_t seqno, daemon* d);
        void start_verify_read(uint64_t seqno, daemon* d);
        void start_verify_write(uint64_t seqno, daemon* d);

        // inter-data center
        std::string generate_log_entry(uint64_t seqno);

        // commit
        void record_commit(daemon* d);
        void record_abort(daemon* d);

        // message sending
        void send_paxos_2a(uint64_t i, daemon* d);
        void send_paxos_2b(uint64_t i, daemon* d);
        void send_response(operation* op, daemon* d);
        void send_tx_begin(operation* op, daemon* d);
        void send_tx_read(operation* op, daemon* d);
        void send_tx_write(operation* op, daemon* d);
        void send_tx_commit(daemon* d);
        void send_tx_abort(daemon* d);
        void send_to_group(std::auto_ptr<e::buffer> msg, uint64_t timestamps[CONSUS_MAX_REPLICATION_FACTOR], daemon* d);
        void send_to_nondurable(uint64_t seqno, std::auto_ptr<e::buffer> msg, uint64_t timestamps[CONSUS_MAX_REPLICATION_FACTOR], daemon* d);

    private:
        const transaction_group m_tg;
        po6::threads::mutex m_mtx;
        uint64_t m_init_timestamp;
        paxos_group m_group;
        paxos_group_id m_dcs[CONSUS_MAX_REPLICATION_FACTOR];
        uint64_t m_dcs_timestamps[CONSUS_MAX_REPLICATION_FACTOR];
        size_t m_dcs_sz;
        state_t m_state;
        uint64_t m_timestamp;
        bool m_prefer_to_commit;
        std::vector<operation> m_ops;
        std::vector<std::pair<comm_id, uint64_t> > m_deferred_2b;

    private:
        transaction(const transaction&);
        transaction& operator = (const transaction&);
};

END_CONSUS_NAMESPACE

#endif // consus_txman_transaction_h_
