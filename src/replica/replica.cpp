/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "replica.h"
#include "mutation.h"
#include "mutation_log.h"
#include "replica_stub.h"
#include "duplication/replica_duplicator_manager.h"
#include "backup/replica_backup_manager.h"
#include "backup/cold_backup_context.h"
#include "bulk_load/replica_bulk_loader.h"
#include "split/replica_split_manager.h"
#include "replica_disk_migrator.h"
#include "runtime/security/access_controller.h"

#include <dsn/utils/latency_tracer.h>
#include <dsn/cpp/json_helper.h>
#include <dsn/dist/replication/replication_app_base.h>
#include <dsn/dist/fmt_logging.h>
#include <dsn/utility/rand.h>
#include <dsn/utility/string_conv.h>
#include <dsn/utility/strings.h>
#include <dsn/tool-api/rpc_message.h>

namespace dsn {
namespace replication {

replica::replica(
    replica_stub *stub, gpid gpid, const app_info &app, const char *dir, bool need_restore)
    : serverlet<replica>("replica"),
      replica_base(gpid, fmt::format("{}@{}", gpid, stub->_primary_address_str), app.app_name),
      _app_info(app),
      _primary_states(
          gpid, stub->options().staleness_for_commit, stub->options().batch_write_disabled),
      _potential_secondary_states(this),
      _cold_backup_running_count(0),
      _cold_backup_max_duration_time_ms(0),
      _cold_backup_max_upload_file_size(0),
      _chkpt_total_size(0),
      _cur_download_size(0),
      _restore_progress(0),
      _restore_status(ERR_OK),
      _duplication_mgr(new replica_duplicator_manager(this)),
      _duplicating(app.duplicating),
      _backup_mgr(new replica_backup_manager(this))
{
    dassert(_app_info.app_type != "", "");
    dassert(stub != nullptr, "");
    _stub = stub;
    _dir = dir;
    _options = &stub->options();
    init_state();
    _config.pid = gpid;
    _bulk_loader = make_unique<replica_bulk_loader>(this);
    _split_mgr = make_unique<replica_split_manager>(this);
    _disk_migrator = make_unique<replica_disk_migrator>(this);

    std::string counter_str = fmt::format("private.log.size(MB)@{}", gpid);
    _counter_private_log_size.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_NUMBER, counter_str.c_str());

    counter_str = fmt::format("recent.write.throttling.delay.count@{}", gpid);
    _counter_recent_write_throttling_delay_count.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_VOLATILE_NUMBER, counter_str.c_str());

    counter_str = fmt::format("recent.write.throttling.reject.count@{}", gpid);
    _counter_recent_write_throttling_reject_count.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_VOLATILE_NUMBER, counter_str.c_str());

    counter_str = fmt::format("recent.read.throttling.delay.count@{}", gpid);
    _counter_recent_read_throttling_delay_count.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_VOLATILE_NUMBER, counter_str.c_str());

    counter_str = fmt::format("recent.read.throttling.reject.count@{}", gpid);
    _counter_recent_read_throttling_reject_count.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_VOLATILE_NUMBER, counter_str.c_str());

    counter_str = fmt::format("dup.disabled_non_idempotent_write_count@{}", _app_info.app_name);
    _counter_dup_disabled_non_idempotent_write_count.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_VOLATILE_NUMBER, counter_str.c_str());

    // init table level latency perf counters
    init_table_level_latency_counters();

    counter_str = fmt::format("backup_request_qps@{}", _app_info.app_name);
    _counter_backup_request_qps.init_app_counter(
        "eon.replica", counter_str.c_str(), COUNTER_TYPE_RATE, counter_str.c_str());

    if (need_restore) {
        // add an extra env for restore
        _extra_envs.insert(
            std::make_pair(backup_restore_constant::FORCE_RESTORE, std::string("true")));
    }

    _access_controller = security::create_replica_access_controller(name());
}

void replica::update_last_checkpoint_generate_time()
{
    _last_checkpoint_generate_time_ms = dsn_now_ms();
    uint64_t max_interval_ms = _options->checkpoint_max_interval_hours * 3600000UL;
    // use random trigger time to avoid flush peek
    _next_checkpoint_interval_trigger_time_ms =
        _last_checkpoint_generate_time_ms + rand::next_u64(max_interval_ms / 2, max_interval_ms);
}

//            //
// Statistics //
//            //

void replica::update_commit_qps(int count)
{
    _stub->_counter_replicas_commit_qps->add((uint64_t)count);
}

void replica::init_state()
{
    _inactive_is_transient = false;
    _is_initializing = false;
    _deny_client_write = false;
    _prepare_list =
        new prepare_list(this,
                         0,
                         _options->max_mutation_count_in_prepare_list,
                         std::bind(&replica::execute_mutation, this, std::placeholders::_1));

    _config.ballot = 0;
    _config.pid.set_app_id(0);
    _config.pid.set_partition_index(0);
    _config.status = partition_status::PS_INACTIVE;
    _primary_states.membership.ballot = 0;
    _create_time_ms = dsn_now_ms();
    _last_config_change_time_ms = _create_time_ms;
    update_last_checkpoint_generate_time();
    _private_log = nullptr;
}

replica::~replica(void)
{
    close();

    if (nullptr != _prepare_list) {
        delete _prepare_list;
        _prepare_list = nullptr;
    }

    dinfo("%s: replica destroyed", name());
}

void replica::on_client_read(dsn::message_ex *request, bool ignore_throttling)
{
    if (!_access_controller->allowed(request)) {
        response_client_read(request, ERR_ACL_DENY);
        return;
    }

    CHECK_REQUEST_IF_SPLITTING(read)

    if (status() == partition_status::PS_INACTIVE ||
        status() == partition_status::PS_POTENTIAL_SECONDARY) {
        response_client_read(request, ERR_INVALID_STATE);
        return;
    }

    if (!ignore_throttling && throttle_read_request(request)) {
        return;
    }

    if (!request->is_backup_request()) {
        // only backup request is allowed to read from a stale replica

        if (status() != partition_status::PS_PRIMARY) {
            response_client_read(request, ERR_INVALID_STATE);
            return;
        }

        // a small window where the state is not the latest yet
        if (last_committed_decree() < _primary_states.last_prepare_decree_on_new_primary) {
            derror_replica("last_committed_decree(%" PRId64
                           ") < last_prepare_decree_on_new_primary(%" PRId64 ")",
                           last_committed_decree(),
                           _primary_states.last_prepare_decree_on_new_primary);
            response_client_read(request, ERR_INVALID_STATE);
            return;
        }
    } else {
        _counter_backup_request_qps->increment();
    }

    uint64_t start_time_ns = dsn_now_ns();
    dassert(_app != nullptr, "");
    _app->on_request(request);

    // If the corresponding perf counter exist, count the duration of this operation.
    // rpc code of request is already checked in message_ex::rpc_code, so it will always be legal
    if (_counters_table_level_latency[request->rpc_code()] != nullptr) {
        _counters_table_level_latency[request->rpc_code()]->set(dsn_now_ns() - start_time_ns);
    }
}

void replica::response_client_read(dsn::message_ex *request, error_code error)
{
    _stub->response_client(get_gpid(), true, request, status(), error);
}

void replica::response_client_write(dsn::message_ex *request, error_code error)
{
    _stub->response_client(get_gpid(), false, request, status(), error);
}

void replica::check_state_completeness()
{
    /* prepare commit durable */
    dassert(max_prepared_decree() >= last_committed_decree(),
            "%" PRId64 " VS %" PRId64 "",
            max_prepared_decree(),
            last_committed_decree());
    dassert(last_committed_decree() >= last_durable_decree(),
            "%" PRId64 " VS %" PRId64 "",
            last_committed_decree(),
            last_durable_decree());
}

void replica::execute_mutation(mutation_ptr &mu)
{
    dinfo("%s: execute mutation %s: request_count = %u",
          name(),
          mu->name(),
          static_cast<int>(mu->client_requests.size()));

    error_code err = ERR_OK;
    decree d = mu->data.header.decree;

    switch (status()) {
    case partition_status::PS_INACTIVE:
        if (_app->last_committed_decree() + 1 == d) {
            err = _app->apply_mutation(mu);
        } else {
            dinfo("%s: mutation %s commit to %s skipped, app.last_committed_decree = %" PRId64,
                  name(),
                  mu->name(),
                  enum_to_string(status()),
                  _app->last_committed_decree());
        }
        break;
    case partition_status::PS_PRIMARY: {
        ADD_POINT(mu->tracer);
        check_state_completeness();
        dassert(_app->last_committed_decree() + 1 == d,
                "app commit: %" PRId64 ", mutation decree: %" PRId64 "",
                _app->last_committed_decree(),
                d);
        err = _app->apply_mutation(mu);
    } break;

    case partition_status::PS_SECONDARY:
        if (!_secondary_states.checkpoint_is_running) {
            check_state_completeness();
            dassert(_app->last_committed_decree() + 1 == d,
                    "%" PRId64 " VS %" PRId64 "",
                    _app->last_committed_decree() + 1,
                    d);
            err = _app->apply_mutation(mu);
        } else {
            dinfo("%s: mutation %s commit to %s skipped, app.last_committed_decree = %" PRId64,
                  name(),
                  mu->name(),
                  enum_to_string(status()),
                  _app->last_committed_decree());

            // make sure private log saves the state
            // catch-up will be done later after checkpoint task is fininished
            dassert(_private_log != nullptr, "");
        }
        break;
    case partition_status::PS_POTENTIAL_SECONDARY:
        if (_potential_secondary_states.learning_status == learner_status::LearningSucceeded ||
            _potential_secondary_states.learning_status ==
                learner_status::LearningWithPrepareTransient) {
            dassert(_app->last_committed_decree() + 1 == d,
                    "%" PRId64 " VS %" PRId64 "",
                    _app->last_committed_decree() + 1,
                    d);
            err = _app->apply_mutation(mu);
        } else {
            dinfo("%s: mutation %s commit to %s skipped, app.last_committed_decree = %" PRId64,
                  name(),
                  mu->name(),
                  enum_to_string(status()),
                  _app->last_committed_decree());

            // prepare also happens with learner_status::LearningWithPrepare, in this case
            // make sure private log saves the state,
            // catch-up will be done later after the checkpoint task is finished
            dassert(_private_log != nullptr, "");
        }
        break;
    case partition_status::PS_PARTITION_SPLIT:
        if (_split_states.is_caught_up) {
            dcheck_eq(_app->last_committed_decree() + 1, d);
            err = _app->apply_mutation(mu);
        }
        break;
    case partition_status::PS_ERROR:
        break;
    default:
        dassert(false, "invalid partition_status, status = %s", enum_to_string(status()));
    }

    dinfo(
        "TwoPhaseCommit, %s: mutation %s committed, err = %s", name(), mu->name(), err.to_string());

    if (err != ERR_OK) {
        handle_local_failure(err);
    }

    if (status() == partition_status::PS_PRIMARY) {
        ADD_CUSTOM_POINT(mu->tracer, "completed");
        mutation_ptr next = _primary_states.write_queue.check_possible_work(
            static_cast<int>(_prepare_list->max_decree() - d));

        if (next) {
            init_prepare(next, false);
        }
    }

    // update table level latency perf-counters for primary partition
    if (partition_status::PS_PRIMARY == status()) {
        uint64_t now_ns = dsn_now_ns();
        for (auto update : mu->data.updates) {
            // If the corresponding perf counter exist, count the duration of this operation.
            // code in update will always be legal
            if (_counters_table_level_latency[update.code] != nullptr) {
                _counters_table_level_latency[update.code]->set(now_ns - update.start_time_ns);
            }
        }
    }
}

mutation_ptr replica::new_mutation(decree decree)
{
    mutation_ptr mu(new mutation());
    mu->data.header.pid = get_gpid();
    mu->data.header.ballot = get_ballot();
    mu->data.header.decree = decree;
    mu->data.header.log_offset = invalid_offset;
    return mu;
}

decree replica::last_durable_decree() const { return _app->last_durable_decree(); }

decree replica::last_flushed_decree() const { return _app->last_flushed_decree(); }

decree replica::last_prepared_decree() const
{
    ballot lastBallot = 0;
    decree start = last_committed_decree();
    while (true) {
        auto mu = _prepare_list->get_mutation_by_decree(start + 1);
        if (mu == nullptr || mu->data.header.ballot < lastBallot || !mu->is_logged())
            break;

        start++;
        lastBallot = mu->data.header.ballot;
    }
    return start;
}

bool replica::verbose_commit_log() const { return _stub->_verbose_commit_log; }

void replica::close()
{
    dassert_replica(status() == partition_status::PS_ERROR ||
                        status() == partition_status::PS_INACTIVE ||
                        _disk_migrator->status() >= disk_migration_status::MOVED,
                    "invalid state(partition_status={}, migration_status={}) when calling "
                    "replica close",
                    enum_to_string(status()),
                    enum_to_string(_disk_migrator->status()));

    uint64_t start_time = dsn_now_ms();

    if (_checkpoint_timer != nullptr) {
        _checkpoint_timer->cancel(true);
        _checkpoint_timer = nullptr;
    }

    if (_app != nullptr) {
        _app->cancel_background_work(true);
    }

    _tracker.cancel_outstanding_tasks();

    cleanup_preparing_mutations(true);
    dassert(_primary_states.is_cleaned(), "primary context is not cleared");

    if (partition_status::PS_INACTIVE == status()) {
        dassert(_secondary_states.is_cleaned(), "secondary context is not cleared");
        dassert(_potential_secondary_states.is_cleaned(),
                "potential secondary context is not cleared");
        dassert(_split_states.is_cleaned(), "partition split context is not cleared");
    }

    // for partition_status::PS_ERROR, context cleanup is done here as they may block
    else {
        bool r = _secondary_states.cleanup(true);
        dassert(r, "secondary context is not cleared");

        r = _potential_secondary_states.cleanup(true);
        dassert(r, "potential secondary context is not cleared");

        r = _split_states.cleanup(true);
        dassert_replica(r, "partition split context is not cleared");
    }

    if (_private_log != nullptr) {
        _private_log->close();
        _private_log = nullptr;
    }

    if (_app != nullptr) {
        std::unique_ptr<replication_app_base> tmp_app = std::move(_app);
        error_code err = tmp_app->close(false);
        if (err != dsn::ERR_OK) {
            dwarn("%s: close app failed, err = %s", name(), err.to_string());
        }
    }

    if (_disk_migrator->status() == disk_migration_status::MOVED) {
        // this will update disk_migration_status::MOVED->disk_migration_status::CLOSED
        _disk_migrator->update_replica_dir();
    } else if (_disk_migrator->status() == disk_migration_status::CLOSED) {
        _disk_migrator.reset();
    }

    _counter_private_log_size.clear();

    // duplication_impl may have ongoing tasks.
    // release it before release replica.
    _duplication_mgr.reset();

    _backup_mgr.reset();

    _bulk_loader.reset();

    _split_mgr.reset();

    ddebug("%s: replica closed, time_used = %" PRIu64 "ms", name(), dsn_now_ms() - start_time);
}

std::string replica::query_manual_compact_state() const
{
    dassert_replica(_app != nullptr, "");
    return _app->query_compact_state();
}

const char *manual_compaction_status_to_string(manual_compaction_status status)
{
    switch (status) {
    case kIdle:
        return "idle";
    case kQueuing:
        return "queuing";
    case kRunning:
        return "running";
    case kFinished:
        return "finished";
    default:
        dassert(false, "invalid status({})", status);
        __builtin_unreachable();
    }
}

manual_compaction_status replica::get_manual_compact_status() const
{
    std::string compact_state = query_manual_compact_state();
    // query_manual_compact_state will return a message like:
    // Case1. last finish at [-]
    // - partition is not manual compaction
    // Case2. last finish at [timestamp], last used {time_used} ms
    // - partition manual compaction finished
    // Case3. last finish at [-], recent enqueue at [timestamp]
    // - partition is in manual compaction queue
    // Case4. last finish at [-], recent enqueue at [timestamp], recent start at [timestamp]
    // - partition is running manual compaction
    if (compact_state.find("recent start at") != std::string::npos) {
        return kRunning;
    } else if (compact_state.find("recent enqueue at") != std::string::npos) {
        return kQueuing;
    } else if (compact_state.find("last used") != std::string::npos) {
        return kFinished;
    } else {
        return kIdle;
    }
}

// Replicas on the server which serves for the same table will share the same perf-counter.
// For example counter `table.level.RPC_RRDB_RRDB_MULTI_PUT.latency(ns)@test_table` is shared by
// all the replicas for `test_table`.
void replica::init_table_level_latency_counters()
{
    int max_task_code = task_code::max();
    _counters_table_level_latency.resize(max_task_code + 1);

    for (int code = 0; code <= max_task_code; code++) {
        _counters_table_level_latency[code] = nullptr;
        if (get_storage_rpc_req_codes().find(task_code(code)) !=
            get_storage_rpc_req_codes().end()) {
            std::string counter_str = fmt::format(
                "table.level.{}.latency(ns)@{}", task_code(code).to_string(), _app_info.app_name);
            _counters_table_level_latency[code] =
                dsn::perf_counters::instance()
                    .get_app_counter("eon.replica",
                                     counter_str.c_str(),
                                     COUNTER_TYPE_NUMBER_PERCENTILES,
                                     counter_str.c_str(),
                                     true)
                    .get();
        }
    }
}

void replica::on_detect_hotkey(const detect_hotkey_request &req, detect_hotkey_response &resp)
{
    _app->on_detect_hotkey(req, resp);
}

uint32_t replica::query_data_version() const
{
    dassert_replica(_app != nullptr, "");
    return _app->query_data_version();
}

} // namespace replication
} // namespace dsn
