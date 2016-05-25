// Copyright (c) 2015, Robert Escriva
// All rights reserved.

// C
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

// POSIX
#include <signal.h>
#include <sys/stat.h>

// STL
#include <algorithm>

// Google Log
#include <glog/logging.h>
#include <glog/raw_logging.h>

// po6
#include <po6/io/fd.h>
#include <po6/path.h>
#include <po6/time.h>

// e
#include <e/atomic.h>
#include <e/daemon.h>
#include <e/daemonize.h>
#include <e/endian.h>
#include <e/guard.h>
#include <e/identity.h>
#include <e/serialization.h>
#include <e/strescape.h>

// BusyBee
#include <busybee_constants.h>

// consus
#include <consus.h>
#include "common/background_thread.h"
#include "common/constants.h"
#include "common/consus.h"
#include "common/lock.h"
#include "common/macros.h"
#include "common/network_msgtype.h"
#include "common/transaction_group.h"
#include "kvs/daemon.h"
#include "kvs/leveldb_datalayer.h"

using consus::daemon;

#define CHECK_UNPACK(MSGTYPE, UNPACKER) \
    do \
    { \
        if (UNPACKER.error()) \
        { \
            network_msgtype CONCAT(_anon, __LINE__)(MSGTYPE); \
            LOG(WARNING) << "received corrupt \"" \
                         << CONCAT(_anon, __LINE__) << "\" message"; \
            return; \
        } \
    } while (0)

uint32_t s_interrupts = 0;
bool s_debug_dump = false;
bool s_debug_mode = false;

static void
exit_on_signal(int /*signum*/)
{
    RAW_LOG(ERROR, "interrupted: exiting");
    e::atomic::increment_32_nobarrier(&s_interrupts, 1);
}

static void
handle_debug_dump(int /*signum*/)
{
    s_debug_dump = true;
}

static void
handle_debug_mode(int /*signum*/)
{
    s_debug_mode = !s_debug_mode;
}

struct daemon::coordinator_callback : public coordinator_link::callback
{
    coordinator_callback(daemon* d);
    virtual ~coordinator_callback() throw ();
    virtual std::string prefix() { return "kvs"; }
    virtual bool new_config(const char* data, size_t data_sz);
    virtual bool has_id(comm_id id);
    virtual po6::net::location address(comm_id id);
    virtual bool is_steady_state(comm_id id);

    private:
        daemon* d;
        coordinator_callback(const coordinator_callback&);
        coordinator_callback& operator = (const coordinator_callback&);
};

class daemon::migration_bgthread : public consus::background_thread
{
    public:
        migration_bgthread(daemon* d);
        virtual ~migration_bgthread() throw ();

    public:
        void new_config();

    protected:
        virtual const char* thread_name();
        virtual bool have_work();
        virtual void do_work();

    private:
        migration_bgthread(const migration_bgthread&);
        migration_bgthread& operator = (const migration_bgthread&);

    private:
        daemon* m_d;
        bool m_have_new_config;
};

daemon :: coordinator_callback :: coordinator_callback(daemon* _d)
    : d(_d)
{
}

daemon :: coordinator_callback :: ~coordinator_callback() throw ()
{
}

static std::vector<std::string>
split_by_newlines(std::string s)
{
    std::vector<std::string> v;

    while (!s.empty())
    {
        size_t idx = s.find_first_of('\n');

        if (idx == std::string::npos)
        {
            v.push_back(s);
            s = "";
        }
        else
        {
            v.push_back(s.substr(0, idx));
            s = s.substr(idx + 1, s.size());
        }
    }

    return v;
}

bool
daemon :: coordinator_callback :: new_config(const char* data, size_t data_sz)
{
    std::auto_ptr<configuration> c(new configuration());
    e::unpacker up(data, data_sz);
    up = up >> *c;

    if (up.error() || up.remain())
    {
        LOG(ERROR) << "received a bad configuration";
        return false;
    }

    configuration* old_config = d->get_config();
    d->m_us.dc = c->get_data_center(d->m_us.id);
    e::atomic::store_ptr_release(&d->m_config, c.release());
    d->m_gc.collect(old_config, e::garbage_collector::free_ptr<configuration>);
    d->m_migrate_thread->new_config();
    LOG(INFO) << "updating to configuration " << d->get_config()->version();

    if (s_debug_mode)
    {
        std::string debug = d->get_config()->dump();
        std::vector<std::string> lines = split_by_newlines(debug);
        LOG(INFO) << "=== begin debug dump of configuration ===";

        for (size_t i = 0; i < lines.size(); ++i)
        {
            LOG(INFO) << lines[i];
        }

        LOG(INFO) << "===  end debug dump of configuration  ===";
    }

    return true;
}

bool
daemon :: coordinator_callback :: has_id(comm_id id)
{
    configuration* c = d->get_config();

    if (c)
    {
        return c->exists(id);
    }

    return false;
}

po6::net::location
daemon :: coordinator_callback :: address(comm_id id)
{
    configuration* c = d->get_config();

    if (c)
    {
        return c->get_address(id);
    }

    return po6::net::location();
}

bool
daemon :: coordinator_callback :: is_steady_state(comm_id id)
{
    configuration* c = d->get_config();

    if (c)
    {
        return c->get_state(id) == kvs_state::ONLINE;
    }

    return false;
}

daemon :: migration_bgthread :: migration_bgthread(daemon* d)
    : background_thread(&d->m_gc)
    , m_d(d)
    , m_have_new_config(false)
{
}

daemon :: migration_bgthread :: ~migration_bgthread() throw ()
{
}

void
daemon :: migration_bgthread :: new_config()
{
    po6::threads::mutex::hold hold(mtx());
    m_have_new_config = true;
    wakeup();
}

const char*
daemon :: migration_bgthread :: thread_name()
{
    return "migration";
}

bool
daemon :: migration_bgthread :: have_work()
{
    return m_have_new_config;
}

void
daemon :: migration_bgthread :: do_work()
{
    {
        po6::threads::mutex::hold hold(mtx());
        m_have_new_config = false;
    }

    configuration* c = m_d->get_config();
    std::vector<partition_id> parts = c->migratable_partitions(m_d->m_us.id);

    for (size_t i = 0; i < parts.size(); ++i)
    {
        migrator_map_t::state_reference msr;
        migrator* m = m_d->m_migrations.get_or_create_state(parts[i], &msr);
        m->externally_work_state_machine(m_d);
    }

    // XXX add a true ticker to drive state machine (call ext_work_sm every X
    // seconds)
    po6::sleep(PO6_SECONDS);

    for (migrator_map_t::iterator it(&m_d->m_migrations); it.valid(); ++it)
    {
        migrator* m = *it;

        if (std::find(parts.begin(), parts.end(), m->state_key()) != parts.end())
        {
            m->externally_work_state_machine(m_d);
        }
        else
        {
            m->terminate();
        }
    }
}

daemon :: daemon()
    : m_us()
    , m_gc()
    , m_busybee_mapper(this)
    , m_busybee()
    , m_coord_cb()
    , m_coord()
    , m_config(NULL)
    , m_threads()
    , m_data()
    , m_repl_rd(&m_gc)
    , m_repl_wr(&m_gc)
    , m_migrations(&m_gc)
    , m_migrate_thread(new migration_bgthread(this))
{
}

daemon :: ~daemon() throw ()
{
    m_gc.collect(get_config(), e::garbage_collector::free_ptr<configuration>);
}

int
daemon :: run(bool background,
              std::string data,
              std::string log,
              std::string pidfile,
              bool has_pidfile,
              bool set_bind_to,
              po6::net::location bind_to,
              bool set_coordinator,
              const char* coordinator,
              unsigned threads)
{
    if (!e::block_all_signals())
    {
        std::cerr << "could not block signals; exiting" << std::endl;
        return EXIT_FAILURE;
    }

    if (!e::daemonize(background, log, "consus-txman-", pidfile, has_pidfile))
    {
        return EXIT_FAILURE;
    }

    if (!e::install_signal_handler(SIGHUP, exit_on_signal) ||
        !e::install_signal_handler(SIGINT, exit_on_signal) ||
        !e::install_signal_handler(SIGTERM, exit_on_signal) ||
        !e::install_signal_handler(SIGQUIT, exit_on_signal) ||
        !e::install_signal_handler(SIGUSR1, handle_debug_dump) ||
        !e::install_signal_handler(SIGUSR2, handle_debug_mode))
    {
        PLOG(ERROR) << "could not install signal handlers";
        return EXIT_FAILURE;
    }

    m_data.reset(new leveldb_datalayer());

    if (!m_data->init(data))
    {
        return EXIT_FAILURE;
    }

    bool saved;
    uint64_t id;
    std::string rendezvous(coordinator);

    if (!e::load_identity(po6::path::join(data, "KVS").c_str(), &saved, &id,
                          set_bind_to, &bind_to, set_coordinator, &rendezvous))
    {
        LOG(ERROR) << "could not load prior identity; exiting";
        return EXIT_FAILURE;
    }

    m_us.id = comm_id(id);
    m_us.bind_to = bind_to;
    bool (coordinator_link::*coordfunc)();

    if (saved)
    {
        coordfunc = &coordinator_link::establish;
    }
    else
    {
        if (!e::generate_token(&id))
        {
            PLOG(ERROR) << "could not read random token from /dev/urandom";
            return EXIT_FAILURE;
        }

        m_us.id = comm_id(id);
        coordfunc = &coordinator_link::initial_registration;
    }

    m_coord_cb.reset(new coordinator_callback(this));
    m_coord.reset(new coordinator_link(rendezvous, m_us.id, m_us.bind_to, m_coord_cb.get()));
    m_coord->allow_reregistration();
    LOG(INFO) << "starting consus kvs-daemon " << m_us.id
              << " on address " << m_us.bind_to;
    LOG(INFO) << "connecting to " << rendezvous;

    if (!(((*m_coord).*coordfunc)()))
    {
        return EXIT_FAILURE;
    }

    assert(get_config());

    if (!e::save_identity(po6::path::join(data, "KVS").c_str(), id, bind_to, rendezvous))
    {
        LOG(ERROR) << "could not save identity; exiting";
        return EXIT_FAILURE;
    }

    m_busybee.reset(new busybee_mta(&m_gc, &m_busybee_mapper, bind_to, id, threads));

    for (size_t i = 0; i < threads; ++i)
    {
        using namespace po6::threads;
        e::compat::shared_ptr<thread> t(new thread(make_obj_func(&daemon::loop, this, i)));
        m_threads.push_back(t);
        t->start();
    }

    m_migrate_thread->start();

    while (e::atomic::increment_32_nobarrier(&s_interrupts, 0) == 0)
    {
        bool debug_mode = s_debug_mode;
        m_coord->maintain_connection();

        if (m_coord->error())
        {
            break;
        }

        if (m_coord->orphaned())
        {
            LOG(ERROR) << "server removed from cluster; exiting";
            break;
        }

        if (s_debug_mode != debug_mode)
        {
            if (s_debug_mode)
            {
                debug_dump();
                LOG(INFO) << "enabling debug mode; will log all state transitions";
                s_debug_dump = false;
            }
            else
            {
                LOG(INFO) << "disabling debug mode; will go back to normal operation";
            }
        }

        if (s_debug_dump)
        {
            debug_dump();
            s_debug_dump = false;
        }
    }

    e::atomic::increment_32_nobarrier(&s_interrupts, 1);
    m_migrate_thread->shutdown();
    m_busybee->shutdown();

    for (size_t i = 0; i < m_threads.size(); ++i)
    {
        m_threads[i]->join();
    }

    LOG(INFO) << "consus is gracefully shutting down";
    return EXIT_SUCCESS;
}

void
daemon :: loop(size_t thread)
{
    size_t core = thread % sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __LINUX__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_t cur = pthread_self();
    int x = pthread_setaffinity_np(cur, sizeof(cpu_set_t), &cpuset);
    assert(x == 0);
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = 0;
    thread_policy_set(mach_thread_self(),
                      THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy,
                      THREAD_AFFINITY_POLICY_COUNT);
#endif

    LOG(INFO) << "network thread " << thread << " started on core " << core;

    sigset_t ss;

    if (sigfillset(&ss) < 0 ||
        pthread_sigmask(SIG_SETMASK, &ss, NULL) < 0)
    {
        std::cerr << "could not block signals" << std::endl;
        return;
    }

    e::garbage_collector::thread_state ts;
    m_gc.register_thread(&ts);
    bool done = false;

    while (!done)
    {
        uint64_t _id;
        std::auto_ptr<e::buffer> msg;
        busybee_returncode rc = m_busybee->recv(&ts, &_id, &msg);

        switch (rc)
        {
            case BUSYBEE_SUCCESS:
                break;
            case BUSYBEE_SHUTDOWN:
                done = true;
                continue;
            case BUSYBEE_DISRUPTED:
            case BUSYBEE_INTERRUPTED:
                continue;
            case BUSYBEE_POLLFAILED:
            case BUSYBEE_ADDFDFAIL:
            case BUSYBEE_TIMEOUT:
            case BUSYBEE_EXTERNAL:
            default:
                LOG(ERROR) << "internal invariants broken; crashing";
                abort();
        }

        comm_id id(_id);
        network_msgtype mt;
        e::unpacker up = msg->unpack_from(BUSYBEE_HEADER_SIZE);
        up = up >> mt;

        if (up.error())
        {
            LOG(WARNING) << "dropping message that has a malformed header";

            if (s_debug_mode)
            {
                LOG(WARNING) << "here's some hex: " << msg->hex();
            }

            continue;
        }

        switch (mt)
        {
            case KVS_REP_RD:
                process_rep_rd(id, msg, up);
                break;
            case KVS_REP_WR:
                process_rep_wr(id, msg, up);
                break;
            case KVS_RAW_RD:
                process_raw_rd(id, msg, up);
                break;
            case KVS_RAW_RD_RESP:
                process_raw_rd_resp(id, msg, up);
                break;
            case KVS_RAW_WR:
                process_raw_wr(id, msg, up);
                break;
            case KVS_RAW_WR_RESP:
                process_raw_wr_resp(id, msg, up);
                break;
            case KVS_LOCK_OP:
                process_lock_op(id, msg, up);
                break;
            case KVS_MIGRATE_SYN:
                process_migrate_syn(id, msg, up);
                break;
            case KVS_MIGRATE_ACK:
                process_migrate_ack(id, msg, up);
                break;
            case CONSUS_NOP:
                break;
            case CLIENT_RESPONSE:
            case UNSAFE_READ:
            case UNSAFE_WRITE:
            case UNSAFE_LOCK_OP:
            case TXMAN_BEGIN:
            case TXMAN_READ:
            case TXMAN_WRITE:
            case TXMAN_COMMIT:
            case TXMAN_ABORT:
            case TXMAN_PAXOS_2A:
            case TXMAN_PAXOS_2B:
            case LV_VOTE_1A:
            case LV_VOTE_1B:
            case LV_VOTE_2A:
            case LV_VOTE_2B:
            case LV_VOTE_LEARN:
            case COMMIT_RECORD:
            case GV_PROPOSE:
            case GV_VOTE_1A:
            case GV_VOTE_1B:
            case GV_VOTE_2A:
            case GV_VOTE_2B:
            case KVS_REP_RD_RESP:
            case KVS_REP_WR_RESP:
            case KVS_LOCK_OP_RESP:
            default:
                LOG(INFO) << "received " << mt << " message which key-value-stores do not process";
                break;
        }

        m_gc.quiescent_state(&ts);
    }

    m_gc.deregister_thread(&ts);
    LOG(INFO) << "network thread shutting down";
}

void
daemon :: process_rep_rd(comm_id id, std::auto_ptr<e::buffer> msg, e::unpacker up)
{
    uint64_t nonce;
    e::slice table;
    e::slice key;
    uint64_t timestamp;
    up = up >> nonce >> table >> key >> timestamp;
    CHECK_UNPACK(KVS_REP_RD, up);

    if (s_debug_mode)
    {
        LOG(INFO) << "replicated read(\"" << e::strescape(table.str()) << "\", \""
                  << e::strescape(key.str()) << "\")";
    }

    // XXX check key meet spec

    while (true)
    {
        uint64_t x = generate_id();
        read_replicator_map_t::state_reference rsr;
        read_replicator* r = m_repl_rd.create_state(x, &rsr);

        if (!r)
        {
            continue;
        }

        r->init(id, nonce, table, key, msg);
        r->externally_work_state_machine(this);
        break;
    }
}

void
daemon :: process_rep_wr(comm_id id, std::auto_ptr<e::buffer> msg, e::unpacker up)
{
    uint64_t nonce;
    uint8_t flags;
    e::slice table;
    e::slice key;
    uint64_t timestamp;
    e::slice value;
    up = up >> nonce >> flags >> table >> key >> timestamp >> value;
    CHECK_UNPACK(KVS_REP_WR, up);

    if (s_debug_mode)
    {
        std::string tmp;
        const char* v = NULL;

        if ((CONSUS_WRITE_TOMBSTONE & flags))
        {
            v = "TOMBSTONE";
        }
        else
        {
            tmp = e::strescape(value.str());
            v = tmp.c_str();
        }

        LOG(INFO) << "replicated write(\"" << e::strescape(table.str()) << "\", \""
                  << e::strescape(key.str()) << "\"@" << timestamp
                  << ", \"" << v << "\")";
    }

    // XXX check key/value meet spec

    while (true)
    {
        uint64_t x = generate_id();
        write_replicator_map_t::state_reference wsr;
        write_replicator* w = m_repl_wr.create_state(x, &wsr);

        if (!w)
        {
            continue;
        }

        w->init(id, nonce, flags, table, key, timestamp, value, msg);
        w->externally_work_state_machine(this);
        break;
    }
}

void
daemon :: process_raw_rd(comm_id id, std::auto_ptr<e::buffer>, e::unpacker up)
{
    uint64_t nonce;
    e::slice table;
    e::slice key;
    uint64_t timestamp;
    up = up >> nonce >> table >> key >> timestamp;
    CHECK_UNPACK(KVS_RAW_RD, up);
    configuration* c = get_config();

    if (s_debug_mode)
    {
        LOG(INFO) << "raw read(\"" << e::strescape(table.str()) << "\", \""
                  << e::strescape(key.str()) << "\")@<=" << timestamp
                  << " nonce=" << nonce;
    }

    // XXX check table exists
    // XXX check key meet spec

    unsigned index = choose_index(table, key);

    if (index == CONSUS_MAX_REPLICATION_FACTOR)
    {
        return;
    }

    comm_id owner1;
    comm_id owner2;
    c->map(m_us.dc, index, &owner1, &owner2);

    e::slice value;
    datalayer::reference* ref = NULL;
    consus_returncode rc = CONSUS_GARBAGE;
    rc = m_data->get(table, key, timestamp, &timestamp, &value, &ref);

    const size_t sz = BUSYBEE_HEADER_SIZE
                    + pack_size(KVS_RAW_RD_RESP)
                    + sizeof(uint64_t)
                    + pack_size(rc)
                    + sizeof(uint64_t)
                    + pack_size(value)
                    + pack_size(owner1);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE)
        << KVS_RAW_RD_RESP << nonce << rc << timestamp << value << owner1;
    send(id, msg);
}

void
daemon :: process_raw_rd_resp(comm_id id, std::auto_ptr<e::buffer> msg, e::unpacker up)
{
    uint64_t nonce;
    consus_returncode rc;
    uint64_t timestamp;
    e::slice value;
    comm_id owner;
    up = up >> nonce >> rc >> timestamp >> value >> owner;
    CHECK_UNPACK(KVS_RAW_RD_RESP, up);
    read_replicator_map_t::state_reference rsr;
    read_replicator* r = m_repl_rd.get_state(nonce, &rsr);

    if (r)
    {
        if (s_debug_mode)
        {
            if (rc == CONSUS_SUCCESS)
            {
                LOG(INFO) << "raw read response nonce=" << nonce << " rc=" << rc
                          << " timestamp=" << timestamp
                          << " value=\"" << e::strescape(value.str()) << "\"" << " from=" << id;
            }
            else if (rc == CONSUS_NOT_FOUND)
            {
                LOG(INFO) << "raw read response nonce=" << nonce << " rc=" << rc
                          << " timestamp=" << timestamp << " from=" << id;
            }
            else
            {
                LOG(INFO) << "raw read response nonce=" << nonce << " rc=" << rc << " from=" << id;
            }
        }

        r->response(id, rc, timestamp, value, owner, msg, this);
    }
    else
    {
        LOG_IF(INFO, s_debug_mode) << "dropped raw read response nonce=" << nonce << " rc=" << rc << " from=" << id;
    }
}

void
daemon :: process_raw_wr(comm_id id, std::auto_ptr<e::buffer>, e::unpacker up)
{
    uint64_t nonce;
    uint8_t flags;
    e::slice table;
    e::slice key;
    uint64_t timestamp;
    e::slice value;
    up = up >> nonce >> flags >> table >> key >> timestamp >> value;
    CHECK_UNPACK(KVS_RAW_WR, up);
    configuration* c = get_config();

    if (s_debug_mode)
    {
        std::string tmp;
        const char* v = NULL;

        if ((CONSUS_WRITE_TOMBSTONE & flags))
        {
            v = "TOMBSTONE";
        }
        else
        {
            tmp = e::strescape(value.str());
            v = tmp.c_str();
        }

        LOG(INFO) << "raw write(\"" << e::strescape(table.str()) << "\", \""
                  << e::strescape(key.str()) << "\"@" << timestamp
                  << ", \"" << v << "\") nonce=" << nonce;
    }

    // XXX check table exists
    // XXX check key/value meet spec

    unsigned index = choose_index(table, key);

    if (index == CONSUS_MAX_REPLICATION_FACTOR)
    {
        return;
    }

    comm_id owner1;
    comm_id owner2;
    c->map(m_us.dc, index, &owner1, &owner2);
    consus_returncode rc = CONSUS_GARBAGE;

    if ((CONSUS_WRITE_TOMBSTONE & flags))
    {
        rc = m_data->del(table, key, timestamp);
    }
    else
    {
        rc = m_data->put(table, key, timestamp, value);
    }

    const size_t sz = BUSYBEE_HEADER_SIZE
                    + pack_size(KVS_RAW_WR_RESP)
                    + sizeof(uint64_t)
                    + pack_size(rc)
                    + pack_size(owner1)
                    + pack_size(owner2);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE) << KVS_RAW_WR_RESP << nonce << rc << owner1 << owner2;
    send(id, msg);
}

void
daemon :: process_raw_wr_resp(comm_id id, std::auto_ptr<e::buffer>, e::unpacker up)
{
    uint64_t nonce;
    consus_returncode rc;
    comm_id owner1;
    comm_id owner2;
    up = up >> nonce >> rc >> owner1 >> owner2;
    CHECK_UNPACK(KVS_RAW_WR_RESP, up);
    write_replicator_map_t::state_reference wsr;
    write_replicator* w = m_repl_wr.get_state(nonce, &wsr);

    if (w)
    {
        LOG_IF(INFO, s_debug_mode) << "raw write response nonce=" << nonce << " rc=" << rc << " from=" << id;
        w->response(id, rc, owner1, owner2, this);
    }
    else
    {
        LOG_IF(INFO, s_debug_mode) << "dropped raw write response nonce=" << nonce << " rc=" << rc << " from=" << id;
    }
}

void
daemon :: process_lock_op(comm_id id, std::auto_ptr<e::buffer> msg, e::unpacker up)
{
    uint64_t nonce;
    e::slice table;
    e::slice key;
    transaction_id txid;
    lock_t type;
    lock_op op;
    up = up >> nonce >> table >> key >> txid >> type >> op;
    CHECK_UNPACK(KVS_LOCK_OP, up);

    LOG(WARNING) << type << " " << op << "(\"" << e::strescape(table.str()) << "\", \""
                 << e::strescape(key.str()) << "\") nonce=" << nonce
                 << "; this is a NOP and not yet implemented";
    msg->pack_at(BUSYBEE_HEADER_SIZE) << KVS_LOCK_OP_RESP << nonce << CONSUS_SUCCESS;
    send(id, msg);
}

void
daemon :: process_migrate_syn(comm_id id, std::auto_ptr<e::buffer> msg, e::unpacker up)
{
    partition_id key;
    version_id version;
    up = up >> key >> version;
    CHECK_UNPACK(KVS_MIGRATE_SYN, up);
    configuration* c = get_config();

    if (c->version() >= version)
    {
        LOG_IF(INFO, s_debug_mode) << "received migration SYN for " << key << "/" << version;
        assert(pack_size(KVS_MIGRATE_SYN) == pack_size(KVS_MIGRATE_ACK));
        msg->pack_at(BUSYBEE_HEADER_SIZE) << KVS_MIGRATE_ACK << key << c->version();
        send(id, msg);
    }
    else
    {
        LOG_IF(INFO, s_debug_mode) << "dropping migration SYN for " << key << "/" << version;
    }
}

void
daemon :: process_migrate_ack(comm_id, std::auto_ptr<e::buffer>, e::unpacker up)
{
    partition_id key;
    version_id version;
    up = up >> key >> version;
    CHECK_UNPACK(KVS_MIGRATE_ACK, up);

    // XXX check source?

    migrator_map_t::state_reference msr;
    migrator* m = m_migrations.get_state(key, &msr);

    if (m)
    {
        m->ack(version, this);
    }
}

consus::configuration*
daemon :: get_config()
{
    return e::atomic::load_ptr_acquire(&m_config);
}

void
daemon :: debug_dump()
{
    LOG(ERROR) << "DEBUG DUMP"; // XXX
}

uint64_t
daemon :: generate_id()
{
    // XXX copy of txman impl
    // XXX don't go out of process
    po6::io::fd fd(open("/dev/urandom", O_RDONLY));
    uint64_t x;
    int ret = fd.xread(&x, sizeof(x));
    assert(ret == 8);
    return x;
}

unsigned
daemon :: choose_index(const e::slice& /*XXX table*/, const e::slice& key)
{
    // XXX use a better scheme
    char buf[sizeof(uint16_t)];
    memset(buf, 0, sizeof(buf));
    memmove(buf, key.data(), key.size() < 2 ? key.size() : 2);
    uint16_t index;
    e::unpack16be(buf, &index);
    return index;
}

void
daemon :: choose_replicas(const e::slice& table, const e::slice& key,
                          comm_id replicas[CONSUS_MAX_REPLICATION_FACTOR],
                          unsigned* num_replicas, unsigned* desired_replication)
{
    *desired_replication = 5U;//XXX
    unsigned index = choose_index(table, key);

    if (index == CONSUS_KVS_PARTITIONS)
    {
        *num_replicas = 0;
        return;
    }

    configuration* c = get_config();
    data_center_id dc = c->get_data_center(m_us.id);

    if (!c->hash(dc, index, replicas, num_replicas))
    {
        *num_replicas = 0;
    }

    if (*num_replicas > *desired_replication)
    {
        *num_replicas = *desired_replication;
    }
}

bool
daemon :: send(comm_id id, std::auto_ptr<e::buffer> msg)
{
    busybee_returncode rc = m_busybee->send(id.get(), msg);

    switch (rc)
    {
        case BUSYBEE_SUCCESS:
            return true;
        case BUSYBEE_DISRUPTED:
            return false;
        case BUSYBEE_SHUTDOWN:
        case BUSYBEE_INTERRUPTED:
        case BUSYBEE_POLLFAILED:
        case BUSYBEE_ADDFDFAIL:
        case BUSYBEE_TIMEOUT:
        case BUSYBEE_EXTERNAL:
        default:
            LOG(ERROR) << "internal invariants broken; crashing";
            abort();
    }
}
