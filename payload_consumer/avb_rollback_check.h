/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UPDATE_ENGINE_PAYLOAD_CONSUMER_AVB_ROLLBACK_CHECK_H_
#define UPDATE_ENGINE_PAYLOAD_CONSUMER_AVB_ROLLBACK_CHECK_H_

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <base/logging.h>
#include <libavb/libavb.h>

#include "update_engine/common/boot_control_interface.h"
#include "update_engine/common/utils.h"
#include "update_engine/payload_consumer/install_plan.h"

namespace chromeos_update_engine {
namespace avb_rollback {

// Read metadata only: payload hashes have already been verified by the caller.
inline bool ReadIndex(const std::string& path, bool has_footer, uint64_t* index) {
  android::base::unique_fd fd(
      TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
  if (fd.get() < 0) {
    PLOG(ERROR) << "Cannot open AVB partition " << path;
    return false;
  }
  const auto size = utils::FileSize(fd.get());
  if (size < static_cast<off64_t>(sizeof(AvbVBMetaImageHeader))) {
    LOG(ERROR) << "AVB partition is too small: " << path;
    return false;
  }
  uint64_t offset = 0;
  uint64_t available = size;
  if (has_footer) {
    AvbFooter raw_footer, footer;
    if (!android::base::ReadFullyAtOffset(fd.get(),
                                         &raw_footer,
                                         sizeof(raw_footer),
                                         size - sizeof(raw_footer)) ||
        !avb_footer_validate_and_byteswap(&raw_footer, &footer)) {
      LOG(ERROR) << "Cannot read or parse AVB footer: " << path;
      return false;
    }
    const uint64_t data_size = size - sizeof(raw_footer);
    if (footer.vbmeta_offset > data_size ||
        footer.vbmeta_size < sizeof(AvbVBMetaImageHeader) ||
        footer.vbmeta_size > data_size - footer.vbmeta_offset ||
        footer.original_image_size > footer.vbmeta_offset) {
      LOG(ERROR) << "AVB footer points outside image: " << path;
      return false;
    }
    offset = footer.vbmeta_offset;
    available = footer.vbmeta_size;
  }
  AvbVBMetaImageHeader raw_header, header;
  if (!android::base::ReadFullyAtOffset(
          fd.get(), &raw_header, sizeof(raw_header), offset) ||
      memcmp(raw_header.magic, AVB_MAGIC, AVB_MAGIC_LEN) != 0) {
    LOG(ERROR) << "Cannot read or parse AVB header: " << path;
    return false;
  }
  avb_vbmeta_image_header_to_host_byte_order(&raw_header, &header);
  available -= sizeof(raw_header);
  if (header.required_libavb_version_major != AVB_VERSION_MAJOR ||
      header.required_libavb_version_minor > AVB_VERSION_MINOR ||
      header.authentication_data_block_size > available ||
      header.auxiliary_data_block_size >
          available - header.authentication_data_block_size ||
      header.authentication_data_block_size % 64 != 0 ||
      header.auxiliary_data_block_size % 64 != 0) {
    LOG(ERROR) << "Invalid AVB header version or block sizes: " << path;
    return false;
  }
  *index = header.rollback_index;
  return true;
}

}  // namespace avb_rollback

inline bool CheckAvbRollbackIndexes(BootControlInterface* boot_control,
                                   const InstallPlan& plan,
                                   bool enabled) {
  if (!enabled) {
    return true;
  }
  const auto current_slot = boot_control->GetCurrentSlot();
  if (current_slot == BootControlInterface::kInvalidSlot ||
      plan.source_slot != current_slot ||
      plan.target_slot == BootControlInterface::kInvalidSlot ||
      plan.target_slot == current_slot) {
    LOG(ERROR) << "Invalid slots for AVB rollback check";
    return false;
  }
  constexpr std::array<const char*, 2> names = {"vbmeta_system", "boot"};
  std::array<uint64_t, 2> source{}, target{};
  for (size_t i = 0; i < names.size(); ++i) {
    const bool not_in_payload = std::none_of(
        plan.partitions.begin(),
        plan.partitions.end(),
        [&](const auto& part) { return part.name == names[i]; });
    for (bool is_target : {false, true}) {
      const auto slot = is_target ? plan.target_slot : current_slot;
      auto device = boot_control->GetPartitionDevice(
          names[i], slot, current_slot, not_in_payload);
      if (!device ||
          !avb_rollback::ReadIndex(device->readonly_device_path,
                                  i == 1,
                                  &(is_target ? target : source)[i])) {
        LOG(ERROR) << "AVB rollback check failed to read " << names[i]
                   << " slot " << BootControlInterface::SlotName(slot);
        return false;
      }
    }
  }
  LOG(INFO) << "AVB rollback indexes: vbmeta_system source=" << source[0]
            << " target=" << target[0] << "; boot source=" << source[1]
            << " target=" << target[1];
  if (target[0] < source[0] || target[1] < source[1]) {
    LOG(ERROR) << "Refusing OTA with lower AVB rollback index";
    return false;
  }
  return true;
}

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_PAYLOAD_CONSUMER_AVB_ROLLBACK_CHECK_H_
