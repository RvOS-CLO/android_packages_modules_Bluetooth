/*
 *
 *  Copyright 2020 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iomanip>
#include <iostream>
#include <map>
#include <vector>

#include "btif/include/btif_hh.h"
#include "hci/include/hci_layer.h"
#include "hci/include/hci_packet_factory.h"
#include "internal_include/stack_config.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "stack/btm/btm_int_types.h"
#include "stack/btm/btm_sco.h"
#include "stack/include/acl_api.h"
#include "stack/include/acl_hci_link_interface.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/hcidefs.h"
#include "stack/l2cap/l2c_int.h"
#include "test/mock/mock_stack_hcic_hcicmds.h"
#include "types/raw_address.h"

namespace mock = test::mock::stack_hcic_hcicmds;

extern tBTM_CB btm_cb;

uint8_t appl_trace_level = BT_TRACE_LEVEL_VERBOSE;
btif_hh_cb_t btif_hh_cb;
tL2C_CB l2cb;

const hci_t* hci_layer_get_interface() { return nullptr; }

void LogMsg(uint32_t trace_set_mask, const char* fmt_str, ...) {}

const std::string kSmpOptions("mock smp options");

bool get_trace_config_enabled(void) { return false; }
bool get_pts_avrcp_test(void) { return false; }
bool get_pts_secure_only_mode(void) { return false; }
bool get_pts_conn_updates_disabled(void) { return false; }
bool get_pts_crosskey_sdp_disable(void) { return false; }
const std::string* get_pts_smp_options(void) { return &kSmpOptions; }
int get_pts_smp_failure_case(void) { return 123; }
config_t* get_all(void) { return nullptr; }
const packet_fragmenter_t* packet_fragmenter_get_interface() { return nullptr; }

stack_config_t mock_stack_config{
    .get_trace_config_enabled = get_trace_config_enabled,
    .get_pts_avrcp_test = get_pts_avrcp_test,
    .get_pts_secure_only_mode = get_pts_secure_only_mode,
    .get_pts_conn_updates_disabled = get_pts_conn_updates_disabled,
    .get_pts_crosskey_sdp_disable = get_pts_crosskey_sdp_disable,
    .get_pts_smp_options = get_pts_smp_options,
    .get_pts_smp_failure_case = get_pts_smp_failure_case,
    .get_all = get_all,
};
const stack_config_t* stack_config_get_interface(void) {
  return &mock_stack_config;
}

std::map<std::string, int> mock_function_count_map;

namespace {

using testing::_;
using testing::DoAll;
using testing::NotNull;
using testing::Pointee;
using testing::Return;
using testing::SaveArg;
using testing::SaveArgPointee;
using testing::StrEq;
using testing::StrictMock;
using testing::Test;

std::string Hex16(int n) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::setw(4) << std::setfill('0') << n;
  return oss.str();
}

class StackBtmTest : public Test {
 public:
 protected:
  void SetUp() override { mock_function_count_map.clear(); }
  void TearDown() override {}
};

TEST_F(StackBtmTest, GlobalLifecycle) {
  get_btm_client_interface().lifecycle.btm_init();
  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, DynamicLifecycle) {
  auto* btm = new tBTM_CB();
  delete btm;
}

TEST_F(StackBtmTest, InformClientOnConnectionSuccess) {
  get_btm_client_interface().lifecycle.btm_init();

  RawAddress bda({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});

  btm_acl_connected(bda, 2, HCI_SUCCESS, false);
  ASSERT_EQ(static_cast<size_t>(1),
            mock_function_count_map.count("BTA_dm_acl_up"));

  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, NoInformClientOnConnectionFail) {
  get_btm_client_interface().lifecycle.btm_init();

  RawAddress bda({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});

  btm_acl_connected(bda, 2, HCI_ERR_NO_CONNECTION, false);
  ASSERT_EQ(static_cast<size_t>(0),
            mock_function_count_map.count("BTA_dm_acl_up"));

  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, default_packet_type) {
  get_btm_client_interface().lifecycle.btm_init();

  btm_cb.acl_cb_.SetDefaultPacketTypeMask(0x4321);
  ASSERT_EQ(0x4321, btm_cb.acl_cb_.DefaultPacketTypes());

  get_btm_client_interface().lifecycle.btm_free();
}

TEST_F(StackBtmTest, change_packet_type) {
  int cnt = 0;
  get_btm_client_interface().lifecycle.btm_init();

  btm_cb.acl_cb_.SetDefaultPacketTypeMask(0xffff);
  ASSERT_EQ(0xffff, btm_cb.acl_cb_.DefaultPacketTypes());

  // Create connection
  RawAddress bda({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
  btm_acl_created(bda, 0x123, HCI_ROLE_CENTRAL, BT_TRANSPORT_BR_EDR);

  uint64_t features = 0xffffffffffffffff;
  acl_process_supported_features(0x123, features);

  uint16_t handle{0};
  uint16_t packet_types{0};

  mock::btsnd_hcic_change_conn_type.body = [&handle, &packet_types](
                                               uint16_t h, uint16_t p) {
    handle = h;
    packet_types = p;
  };
  btm_set_packet_types_from_address(bda, 0x55aa);
  ASSERT_EQ(++cnt, mock_function_count_map["btsnd_hcic_change_conn_type"]);
  ASSERT_EQ(0x123, handle);
  ASSERT_EQ(Hex16(0x4400 | HCI_PKT_TYPES_MASK_DM1), Hex16(packet_types));

  btm_set_packet_types_from_address(bda, 0xffff);
  ASSERT_EQ(++cnt, mock_function_count_map["btsnd_hcic_change_conn_type"]);
  ASSERT_EQ(0x123, handle);
  ASSERT_EQ(Hex16(0xcc00 | HCI_PKT_TYPES_MASK_DM1 | HCI_PKT_TYPES_MASK_DH1),
            Hex16(packet_types));

  btm_set_packet_types_from_address(bda, 0x0);
  ASSERT_EQ(0x123, handle);
  ASSERT_EQ(Hex16(0xcc18), Hex16(packet_types));

  mock::btsnd_hcic_change_conn_type = {};
  get_btm_client_interface().lifecycle.btm_free();
}

TEST(ScoTest, make_sco_packet) {
  std::vector<uint8_t> data = {10, 20, 30};
  uint16_t handle = 0xab;
  BT_HDR* p = btm_sco_make_packet(data, handle);
  ASSERT_EQ(p->event, BT_EVT_TO_LM_HCI_SCO);
  ASSERT_EQ(p->len, 3 + data.size());
  ASSERT_EQ(p->data[0], 0xab);
  ASSERT_EQ(p->data[1], 0);
  ASSERT_EQ(p->data[2], 3);
  ASSERT_EQ(p->data[3], 10);
  ASSERT_EQ(p->data[4], 20);
  ASSERT_EQ(p->data[5], 30);
  osi_free(p);
}

}  // namespace
