/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <dirent.h>
#include <fcntl.h>
#include <libdm/dm.h>
#include <libdm/dm_table.h>
#include <libdm/dm_target.h>
#include <libdm/loop_control.h>
#include <liblp/builder.h>
#include <liblp/liblp.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using android::base::GetProperty;
using android::base::ReadFileToString;
using android::base::Realpath;
using android::base::SetProperty;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::dm::LoopControl;

namespace {

constexpr off_t kMiscVirtualAbOffset = 32768;
constexpr uint32_t kMiscVirtualAbMagic = 0x56740ab0U;
constexpr uint64_t kOplusStubBytes = 335872ULL;

struct __attribute__((packed)) MiscVirtualAbMessage {
    uint8_t version;
    uint32_t magic;
    uint8_t merge_status;
    uint8_t source_slot;
    uint8_t reserved[57];
};

std::string ParseKernelVersionPrefix(const std::string& release) {
    size_t i = 0;
    while (i < release.size() &&
           ((release[i] >= '0' && release[i] <= '9') || release[i] == '.')) {
        ++i;
    }
    return release.substr(0, i);
}

bool EndsWith(const std::string& str, const std::string& suffix) {
    if (str.size() <= suffix.size()) {
        return false;
    }
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool MountMetadataIfNeeded(bool* mounted) {
    *mounted = false;
    struct stat st_meta{};
    struct stat st_root{};
    if (stat("/metadata/.", &st_meta) == 0 && stat("/.", &st_root) == 0 &&
        st_meta.st_dev != st_root.st_dev) {
        return true;
    }

    mkdir("/metadata", 0755);
    const char* devs[] = {
        "/dev/block/by-name/metadata",
        "/dev/block/bootdevice/by-name/metadata",
    };
    for (const char* dev : devs) {
        if (mount(dev, "/metadata", "f2fs", MS_RDONLY | MS_NOATIME, "discard") == 0 ||
            mount(dev, "/metadata", "f2fs", MS_NOATIME, "") == 0) {
            *mounted = true;
            return true;
        }
    }
    return false;
}

bool HasPendingVabOta() {
    const char* misc_paths[] = {
        "/dev/block/by-name/misc",
        "/dev/block/bootdevice/by-name/misc",
    };
    for (const char* path : misc_paths) {
        unique_fd fd(open(path, O_RDONLY | O_CLOEXEC));
        if (fd.get() >= 0) {
            MiscVirtualAbMessage msg{};
            if (pread(fd.get(), &msg, sizeof(msg), kMiscVirtualAbOffset) ==
                static_cast<ssize_t>(sizeof(msg))) {
                if (msg.magic == kMiscVirtualAbMagic &&
                    (msg.merge_status == 2 || msg.merge_status == 3)) {
                    return true;
                }
            }
            break;
        }
    }

    bool mounted_meta = false;
    MountMetadataIfNeeded(&mounted_meta);

    bool pending = false;
    std::string state;
    if (ReadFileToString("/metadata/ota/state", &state) && state.size() >= 2 &&
        static_cast<uint8_t>(state[0]) == 0x08 &&
        static_cast<uint8_t>(state[1]) >= 1 &&
        static_cast<uint8_t>(state[1]) <= 4) {
        pending = true;
    } else if (access("/metadata/gsi/ota/lp_metadata", F_OK) == 0) {
        pending = true;
    }

    if (mounted_meta) {
        umount2("/metadata", MNT_DETACH);
    }
    return pending;
}

bool HasUnmergedTargetSlotVab(const std::string& slot_suffix, uint8_t cur_slot) {
    const char* misc_paths[] = {
        "/dev/block/by-name/misc",
        "/dev/block/bootdevice/by-name/misc",
    };
    for (const char* path : misc_paths) {
        unique_fd fd(open(path, O_RDONLY | O_CLOEXEC));
        if (fd.get() >= 0) {
            MiscVirtualAbMessage msg{};
            if (pread(fd.get(), &msg, sizeof(msg), kMiscVirtualAbOffset) ==
                static_cast<ssize_t>(sizeof(msg))) {
                if (msg.magic == kMiscVirtualAbMagic &&
                    (msg.merge_status == 2 || msg.merge_status == 3) &&
                    msg.source_slot != cur_slot) {
                    return true;
                }
            }
            break;
        }
    }

    bool mounted_meta = false;
    MountMetadataIfNeeded(&mounted_meta);

    bool unmerged = false;
    std::string state;
    std::string snap_boot;
    if (ReadFileToString("/metadata/ota/snapshot-boot", &snap_boot)) {
        while (!snap_boot.empty() &&
               (snap_boot.back() == '\n' || snap_boot.back() == '\r')) {
            snap_boot.pop_back();
        }
        if ((snap_boot == "_a" || snap_boot == "_b") && snap_boot != slot_suffix) {
            if (ReadFileToString("/metadata/ota/state", &state) && state.size() >= 2 &&
                static_cast<uint8_t>(state[0]) == 0x08 &&
                static_cast<uint8_t>(state[1]) >= 1 &&
                static_cast<uint8_t>(state[1]) <= 4) {
                unmerged = true;
            } else if (access("/metadata/gsi/ota/lp_metadata", F_OK) == 0) {
                unmerged = true;
            }
        }
    }

    if (mounted_meta) {
        umount2("/metadata", MNT_DETACH);
    }
    return unmerged;
}

bool PwriteAll(int fd, const char* buf, size_t len, off_t offset) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = pwrite(fd, buf + written, len - written,
                           offset + static_cast<off_t>(written));
        if (n <= 0) {
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool WritePartitionImage(int super_fd, const android::fs_mgr::Partition* part,
                         const std::string& image) {
    size_t written = 0;
    for (const auto& extent : part->extents()) {
        auto* linear = extent->AsLinearExtent();
        if (linear == nullptr || linear->device_index() > 0) {
            return false;
        }
        uint64_t ext_bytes = linear->num_sectors() * 512ULL;
        off_t phys_offset = static_cast<off_t>(linear->physical_sector() * 512ULL);
        if (written < image.size()) {
            size_t chunk = std::min<uint64_t>(ext_bytes, image.size() - written);
            if (!PwriteAll(super_fd, image.data() + written, chunk, phys_offset)) {
                return false;
            }
            written += chunk;
        }
    }
    return written == image.size();
}

void ActivateLinearDm(DeviceMapper& dm, const std::string& super_dev,
                      const android::fs_mgr::Partition* part) {
    if (part == nullptr || part->size() == 0) {
        return;
    }
    if (dm.GetState(part->name()) == DmDeviceState::ACTIVE) {
        return;
    }

    DmTable table;
    table.set_readonly(true);
    uint64_t logical_sector = 0;
    for (const auto& extent : part->extents()) {
        auto* linear = extent->AsLinearExtent();
        if (linear == nullptr) {
            return;
        }
        table.Emplace<DmTargetLinear>(logical_sector, linear->num_sectors(),
                                      super_dev, linear->physical_sector());
        logical_sector += linear->num_sectors();
    }
    if (logical_sector == 0) {
        return;
    }
    dm.CreateDevice(part->name(), table);
}

// Stock OxygenOS / ColorOS full OTAs omit my_company and my_preload from
// payload.bin while still requiring both in first_stage_mount. Ensure valid
// EROFS+AVB stubs exist on the active super metadata slot when needed.
bool EnsureOplusSuperStubs(bool precreate_for_ota, uint8_t cur_slot,
                           const std::string& slot_suffix) {
    if (HasPendingVabOta()) {
        return false;
    }

    std::string super_dev;
    const char* super_paths[] = {
        "/dev/block/by-name/super",
        "/dev/block/bootdevice/by-name/super",
    };
    for (int attempt = 0; attempt < 5 && super_dev.empty(); ++attempt) {
        for (const char* path : super_paths) {
            if (Realpath(path, &super_dev)) {
                break;
            }
        }
        if (super_dev.empty()) {
            usleep(50000);
        }
    }
    if (super_dev.empty()) {
        return false;
    }

    auto builder = android::fs_mgr::MetadataBuilder::New(super_dev, cur_slot);
    if (builder == nullptr) {
        return false;
    }

    std::string trigger_name = (precreate_for_ota ? "system" : "my_product") + slot_suffix;
    auto* trigger = builder->FindPartition(trigger_name);
    if (trigger == nullptr || trigger->size() == 0) {
        return false;
    }
    std::string group = trigger->group_name();

    struct StubSpec {
        const char* base_name;
        const char* image_path;
        bool repaired;
        std::string image_data;
    } specs[2] = {
        {"my_company", "/system/etc/oplus_my_company.img", false, ""},
        {"my_preload", "/system/etc/oplus_my_preload.img", false, ""},
    };

    bool any_needed = false;
    for (auto& spec : specs) {
        std::string part_name = std::string(spec.base_name) + slot_suffix;
        auto* part = builder->FindPartition(part_name);
        if (part == nullptr || part->size() < kOplusStubBytes) {
            if (!ReadFileToString(spec.image_path, &spec.image_data) ||
                spec.image_data.size() != kOplusStubBytes) {
                return false;
            }
            if (part != nullptr) {
                builder->RemovePartition(part_name);
            }
            part = builder->AddPartition(part_name, group, LP_PARTITION_ATTR_READONLY);
            if (part == nullptr || !builder->ResizePartition(part, kOplusStubBytes)) {
                if (part != nullptr) {
                    builder->RemovePartition(part_name);
                }
                part = builder->AddPartition(part_name, "default", LP_PARTITION_ATTR_READONLY);
                if (part == nullptr || !builder->ResizePartition(part, kOplusStubBytes)) {
                    return false;
                }
            }
            spec.repaired = true;
            any_needed = true;
        }
    }

    auto& dm = DeviceMapper::Instance();
    if (!any_needed) {
        if (!precreate_for_ota) {
            for (const auto& spec : specs) {
                ActivateLinearDm(dm, super_dev,
                                 builder->FindPartition(std::string(spec.base_name) + slot_suffix));
            }
        }
        return true;
    }

    unique_fd super_fd(open(super_dev.c_str(), O_RDWR | O_CLOEXEC));
    if (super_fd.get() < 0) {
        return false;
    }
    int read_only = 0;
    ioctl(super_fd.get(), BLKROSET, &read_only);

    for (const auto& spec : specs) {
        if (spec.repaired) {
            auto* part = builder->FindPartition(std::string(spec.base_name) + slot_suffix);
            if (part == nullptr || !WritePartitionImage(super_fd.get(), part, spec.image_data)) {
                return false;
            }
        }
    }

    if (fsync(super_fd.get()) < 0) {
        return false;
    }
    auto exported = builder->Export();
    if (exported == nullptr) {
        return false;
    }
    if (!android::fs_mgr::UpdatePartitionTable(super_dev, *exported, cur_slot)) {
        return false;
    }

    if (!precreate_for_ota) {
        for (const auto& spec : specs) {
            ActivateLinearDm(dm, super_dev,
                             builder->FindPartition(std::string(spec.base_name) + slot_suffix));
        }
    }
    std::fprintf(stderr, "aston-vab-guard: ensured Oplus super stubs on %s\n",
                 slot_suffix.c_str());
    return true;
}

std::string ReadKoVermagicPrefix(const std::string& path) {
    std::string data;
    if (!ReadFileToString(path, &data)) {
        return "";
    }
    const std::string tag = "vermagic=";
    size_t pos = data.find(tag);
    if (pos == std::string::npos) {
        return "";
    }
    return ParseKernelVersionPrefix(data.substr(pos + tag.size(), 32));
}

std::string ProbePartitionVermagic(const std::string& dm_name) {
    std::string block_dev = "/dev/block/mapper/" + dm_name;
    if (access(block_dev.c_str(), R_OK) < 0) {
        return "";
    }
    mkdir("/tmp/.aston_mod_chk", 0700);
    if (mount(block_dev.c_str(), "/tmp/.aston_mod_chk", "erofs", MS_RDONLY, nullptr) < 0 &&
        mount(block_dev.c_str(), "/tmp/.aston_mod_chk", "ext4", MS_RDONLY, nullptr) < 0) {
        return "";
    }

    std::string ver = ReadKoVermagicPrefix("/tmp/.aston_mod_chk/lib/modules/msm_kgsl.ko");
    if (ver.empty()) {
        DIR* dir = opendir("/tmp/.aston_mod_chk/lib/modules");
        if (dir != nullptr) {
            while (struct dirent* entry = readdir(dir)) {
                std::string name(entry->d_name);
                if (EndsWith(name, ".ko")) {
                    ver = ReadKoVermagicPrefix("/tmp/.aston_mod_chk/lib/modules/" + name);
                    if (!ver.empty()) {
                        break;
                    }
                }
            }
            closedir(dir);
        }
    }

    umount2("/tmp/.aston_mod_chk", MNT_DETACH);
    return ver;
}

bool AttachZeroLoopDevice(std::string* loop_dev) {
    const char* zero_path = "/tmp/.aston_vab_zero";
    unique_fd fd(open(zero_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (fd.get() < 0) {
        return false;
    }
    if (ftruncate(fd.get(), 65536) < 0) {
        return false;
    }
    LoopControl loop_ctl;
    return loop_ctl.Attach(fd.get(), std::chrono::milliseconds(5000), loop_dev);
}

void ShieldDmDevices(DeviceMapper& dm, const std::vector<std::string>& names,
                     const std::string& loop_dev) {
    for (const auto& name : names) {
        DmTable table;
        table.set_readonly(true);
        table.Emplace<DmTargetLinear>(0, 128, loop_dev, 0);
        if (dm.LoadTable(name, table)) {
            dm.ChangeState(name, DmDeviceState::SUSPENDED);
            dm.ChangeState(name, DmDeviceState::ACTIVE);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string slot_suffix = GetProperty("ro.boot.slot_suffix", "");
    uint8_t cur_slot = 0;
    if (slot_suffix == "_a") {
        cur_slot = 0;
    } else if (slot_suffix == "_b") {
        cur_slot = 1;
    } else {
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "--repair") {
        EnsureOplusSuperStubs(false, cur_slot, slot_suffix);
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--precreate-oplus") {
        EnsureOplusSuperStubs(true, cur_slot, slot_suffix);
        return 0;
    }

    bool unmerged = HasUnmergedTargetSlotVab(slot_suffix, cur_slot);
    if (!unmerged) {
        EnsureOplusSuperStubs(false, cur_slot, slot_suffix);
    }

    auto& dm = DeviceMapper::Instance();
    std::vector<DeviceMapper::DmBlockDevice> devices;
    if (!dm.GetAvailableDevices(&devices)) {
        return 0;
    }

    std::vector<std::string> slot_devices;
    for (const auto& dev : devices) {
        const std::string& name = dev.name();
        if (name == "userdata") {
            continue;
        }
        if (EndsWith(name, slot_suffix) && !EndsWith(name, "-cow")) {
            slot_devices.push_back(name);
        }
    }
    if (slot_devices.empty()) {
        return 0;
    }

    bool shield_gpu_egl = false;
    if (unmerged) {
        std::string loop_dev;
        if (AttachZeroLoopDevice(&loop_dev)) {
            ShieldDmDevices(dm, slot_devices, loop_dev);
            SetProperty("aston.vab.unmerged", "1");
            shield_gpu_egl = true;
            std::fprintf(stderr, "aston-vab-guard: shielded %zu unmerged VAB devices on %s\n",
                         slot_devices.size(), slot_suffix.c_str());
        }
    } else {
        struct utsname uts{};
        if (uname(&uts) == 0) {
            std::string running_ver = ParseKernelVersionPrefix(uts.release);
            std::string vendor_ver = ProbePartitionVermagic("vendor" + slot_suffix);
            std::string dlkm_ver = ProbePartitionVermagic("vendor_dlkm" + slot_suffix);
            bool bad_vendor = (!vendor_ver.empty() && vendor_ver != running_ver);
            bool bad_dlkm = (!dlkm_ver.empty() && dlkm_ver != running_ver);
            if (bad_vendor || bad_dlkm) {
                std::string loop_dev;
                if (AttachZeroLoopDevice(&loop_dev)) {
                    std::vector<std::string> targets;
                    if (bad_vendor) {
                        targets.push_back("vendor" + slot_suffix);
                    }
                    if (bad_dlkm) {
                        targets.push_back("vendor_dlkm" + slot_suffix);
                        targets.push_back("system_dlkm" + slot_suffix);
                    }
                    ShieldDmDevices(dm, targets, loop_dev);
                    SetProperty("aston.kmod.mismatch", "1");
                    shield_gpu_egl = true;
                    std::fprintf(stderr, "aston-vab-guard: shielded mismatched kmod (%s/%s vs %s)\n",
                                 vendor_ver.c_str(), dlkm_ver.c_str(), running_ver.c_str());
                }
            }
        }
    }

    if (shield_gpu_egl && access("/dev/kgsl-3d0", F_OK) < 0) {
        rename("/vendor/lib64/egl/libEGL_adreno.so",
               "/vendor/lib64/egl/libEGL_adreno.so.off");
    }
    return 0;
}
