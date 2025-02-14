// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <dsn/cpp/json_helper.h>
#include <dsn/utility/filesystem.h>
#include <gtest/gtest.h>

#include "common/backup_utils.h"
#include "meta/meta_service.h"
#include "meta/server_state.h"
#include "meta_test_base.h"

namespace dsn {
namespace replication {

class server_state_restore_test : public meta_test_base
{
public:
    server_state_restore_test()
        : _mock_backup_id(dsn_now_ms()),
          _old_app_name("test_table"),
          _new_app_name("new_table"),
          _cluster_name("onebox"),
          _provider("local_service")
    {
    }

    void SetUp() override
    {
        meta_test_base::SetUp();

        // create an app with 8 partitions.
        create_app(_old_app_name);
    }

    void test_restore_app_info(const std::string user_specified_restore_path = "")
    {
        int64_t old_app_id;
        dsn::blob app_info_data;
        {
            zauto_read_lock l;
            _ss->lock_read(l);
            const std::shared_ptr<app_state> &app = _ss->get_app(_old_app_name);
            old_app_id = app->app_id;
            app_info_data = dsn::json::json_forwarder<app_info>::encode(*app);
        }

        configuration_restore_request req;
        req.app_id = old_app_id;
        req.app_name = _old_app_name;
        req.new_app_name = _new_app_name;
        req.time_stamp = _mock_backup_id;
        req.cluster_name = _cluster_name;
        req.backup_provider_name = _provider;
        if (!user_specified_restore_path.empty()) {
            req.__set_restore_path(user_specified_restore_path);
        }
        int32_t new_app_id = _ss->next_app_id();

        dsn::message_ex *msg = dsn::message_ex::create_request(RPC_CM_START_RESTORE);
        dsn::marshall(msg, req);
        auto pair = _ss->restore_app_info(msg, req, app_info_data);

        ASSERT_EQ(ERR_OK, pair.first);
        const std::shared_ptr<app_state> &new_app = pair.second;
        ASSERT_EQ(new_app_id, new_app->app_id);
        ASSERT_EQ(_new_app_name, new_app->app_name);
        ASSERT_EQ(app_status::AS_CREATING, new_app->status);

        // check app_envs
        auto it = new_app->envs.find(backup_restore_constant::BLOCK_SERVICE_PROVIDER);
        ASSERT_NE(new_app->envs.end(), it);
        ASSERT_EQ(_provider, it->second);
        it = new_app->envs.find(backup_restore_constant::CLUSTER_NAME);
        ASSERT_NE(new_app->envs.end(), it);
        ASSERT_EQ(_cluster_name, it->second);
        it = new_app->envs.find(backup_restore_constant::APP_NAME);
        ASSERT_NE(new_app->envs.end(), it);
        ASSERT_EQ(_old_app_name, it->second);
        it = new_app->envs.find(backup_restore_constant::APP_ID);
        ASSERT_NE(new_app->envs.end(), it);
        ASSERT_EQ(std::to_string(old_app_id), it->second);
        it = new_app->envs.find(backup_restore_constant::BACKUP_ID);
        ASSERT_NE(new_app->envs.end(), it);
        ASSERT_EQ(std::to_string(_mock_backup_id), it->second);
        if (!user_specified_restore_path.empty()) {
            it = new_app->envs.find(backup_restore_constant::RESTORE_PATH);
            ASSERT_NE(new_app->envs.end(), it);
            ASSERT_EQ(user_specified_restore_path, it->second);
        }
    }

protected:
    int64_t _mock_backup_id;
    const std::string _old_app_name;
    const std::string _new_app_name;
    const std::string _cluster_name;
    const std::string _provider;
};

TEST_F(server_state_restore_test, test_restore_app) { test_restore_app_info(); }

TEST_F(server_state_restore_test, test_restore_app_with_specific_path)
{
    test_restore_app_info("test_path");
}

} // namespace replication
} // namespace dsn
