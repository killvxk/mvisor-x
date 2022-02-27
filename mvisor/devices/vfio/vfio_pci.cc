/* 
 * MVisor VFIO for vGPU
 * Copyright (C) 2022 Terrence <terrence@tenclass.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "vfio_pci.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include "device_manager.h"
#include "machine.h"
#include "logger.h"

VfioPci::VfioPci() {
  devfn_ = PCI_MAKE_DEVFN(7, 0);
  for (auto &interrupt : interrupts_) {
    interrupt.event_fd = -1;
  }
}

VfioPci::~VfioPci() {

}

void VfioPci::Connect() {
  PciDevice::Connect();

  if (!has_key("sysfs")) {
    MV_PANIC("Please specify 'sysfs' for vfio-pci, like '/sys/bus/mdev/devices/xxx'");
  }
  sysfs_path_ = std::get<std::string>(key_values_["sysfs"]);

  SetupVfioGroup();
  SetupVfioContainer();
  SetupVfioDevice();
  SetupPciConfiguration();
  SetupPciInterrupts();
  SetupGfxPlane();
  SetupDmaMaps();
}

void VfioPci::Disconnect() {
  if (memory_listener_) {
    auto mm = manager_->machine()->memory_manager();
    mm->UnregisterMemoryListener(&memory_listener_);
  }
  for (auto &interrupt : interrupts_) {
    if (interrupt.event_fd > 0) {
      manager_->io()->StopPolling(interrupt.event_fd);
      safe_close(&interrupt.event_fd);
    }
  }
  safe_close(&device_fd_);
  safe_close(&container_fd_);
  safe_close(&group_fd_);
  PciDevice::Disconnect();
}

void VfioPci::Reset() {
  /* reset vfio device */
  if (device_fd_ > 0 && (device_info_.flags & VFIO_DEVICE_FLAGS_RESET)) {
    if (ioctl(device_fd_, VFIO_DEVICE_RESET) < 0) {
      MV_PANIC("failed to reset device %s", name_);
    }
  }

  PciDevice::Reset();
}

void VfioPci::SetupPciConfiguration() {
  /* Read PCI configuration from device */
  auto &config_region = regions_[VFIO_PCI_CONFIG_REGION_INDEX];
  size_t config_size = PCI_DEVICE_CONFIG_SIZE;
  MV_ASSERT(config_region.size >= config_size);
  auto ret = pread(device_fd_, pci_header_.data, config_size, config_region.offset);
  if (ret < (int)config_size) {
    MV_PANIC("failed to read device config space, ret=%d", ret);
  }
  /* Disable IRQ, use MSI instead, should we update the vfio device ??? */
  pci_header_.irq_pin = 0;
  /* Multifunction is not supported yet */
  pci_header_.header_type &= ~PCI_MULTI_FUNCTION;
  MV_ASSERT(pci_header_.header_type == PCI_HEADER_TYPE_NORMAL);
  pci_header_.class_code = 0x030200;

  /* Setup bars */
  for (uint8_t i = 0; i < VFIO_PCI_ROM_REGION_INDEX; i++) {
    auto &bar_region = regions_[i];
    if (!bar_region.size)
      continue;
    auto &bar = pci_header_.bars[i];
    if (bar & PCI_BASE_ADDRESS_SPACE_IO) {
      AddPciBar(i, bar_region.size, kIoResourceTypePio);
    } else {
      /* 64bit bar is not supported yet */
      if (bar & PCI_BASE_ADDRESS_MEM_TYPE_64) {
        bar &= ~PCI_BASE_ADDRESS_MEM_TYPE_64;
      }
      AddPciBar(i, bar_region.size, kIoResourceTypeMmio);
    }
  }

  /* Setup capabilites */
  if (pci_header_.status & PCI_STATUS_CAP_LIST) {
    uint pos = pci_header_.capability & ~3;
    while (pos) {
      auto cap = (PciCapabilityHeader*)(pci_header_.data + pos);
      switch (cap->type)
      {
      case PCI_CAP_ID_MSI: {
        /* Only support 64bit MSI currently */
        auto msi_cap = (MsiCapability64*)cap;
        MV_ASSERT(!(msi_cap->control & PCI_MSI_FLAGS_MASKBIT));
        MV_ASSERT(msi_cap->control & PCI_MSI_FLAGS_64BIT);
        msi_config_.is_64bit = msi_cap->control & PCI_MSI_FLAGS_64BIT;
        msi_config_.offset = pos;
        msi_config_.is_msix = false;
        msi_config_.length = sizeof(*msi_cap);
        msi_config_.msi64 = msi_cap;
        break;
      }
      case PCI_CAP_ID_MSIX:
        MV_PANIC("FIXME: not implemented vfio msix");
        break;
      case PCI_CAP_ID_VNDR:
        break;
      default:
        MV_LOG("unhandled capability=0x%x", cap->type);
        break;
      }
      pos = cap->next;
    }
  }

  /* Update changes to device */
  pwrite(device_fd_, pci_header_.data, config_size, config_region.offset);
}

void VfioPci::SetupGfxPlane() {
  vfio_device_gfx_plane_info gfx_plane_info = {
    .argsz = sizeof(gfx_plane_info),
    .flags = VFIO_GFX_PLANE_TYPE_PROBE | VFIO_GFX_PLANE_TYPE_REGION
  };
  auto ret = ioctl(device_fd_, VFIO_DEVICE_QUERY_GFX_PLANE, &gfx_plane_info);
  if (ret == 0) {
    /* Register GFX plane */
  }
}

void VfioPci::MapDmaPages(const MemorySlot* slot) {
  vfio_iommu_type1_dma_map dma_map = {
    .argsz = sizeof(dma_map),
    .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    .vaddr = slot->hva,
    .iova = slot->begin,
    .size = slot->end - slot->begin
  };
  if (ioctl(container_fd_, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
    MV_PANIC("failed to map 0x%lx-0x%lx", slot->begin, slot->end);
  }
  if (debug_) {
    MV_LOG("map dma 0x%lx-0x%lx", slot->begin, slot->end);
  }
}

void VfioPci::UnmapDmaPages(const MemorySlot* slot) {
  vfio_iommu_type1_dma_unmap dma_ummap = {
    .argsz = sizeof(dma_ummap),
    .iova = slot->begin,
    .size = slot->end - slot->begin
  };
  ioctl(container_fd_, VFIO_IOMMU_UNMAP_DMA, &dma_ummap);
  if (debug_) {
    MV_LOG("unmap dma 0x%lx-0x%lx", slot->begin, slot->end);
  }
}

void VfioPci::SetupDmaMaps() {
  auto mm = manager_->machine()->memory_manager();

  /* Map all current slots */
  for (auto slot : mm->GetMemoryFlatView()) {
    if (slot->region->type == kMemoryTypeRam) {
      MapDmaPages(slot);
    }
  }

  /* Add memory listener to keep DMA maps synchronized */
  memory_listener_ = mm->RegisterMemoryListener([this](auto slot, bool unmap) {
    if (slot->region->type == kMemoryTypeRam) {
      if (unmap) {
        UnmapDmaPages(slot);
      } else {
        MapDmaPages(slot);
      }
    }
  });
}

void VfioPci::SetupVfioGroup() {
  /* Get VFIO group id from device path */
  char path[1024];
  auto len = readlink((sysfs_path_ + "/iommu_group").c_str(), path, 1024);
  if (len < 0) {
    MV_PANIC("failed to read iommu_group");
  }
  path[len] = 0;
  if (sscanf(basename(path), "%d", &group_id_) != 1) {
    MV_PANIC("failed to get group id from %s", path);
  }
  
  /* Open group */
  sprintf(path, "/dev/vfio/%d", group_id_);
  group_fd_ = open(path, O_RDWR);
  if (group_fd_ < 0) {
    MV_PANIC("failed to open %s", path);
  }

  /* Check if it is OK */
  vfio_group_status status = { .argsz = sizeof(status) };
  MV_ASSERT(ioctl(group_fd_, VFIO_GROUP_GET_STATUS, &status) == 0);
  if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
    MV_PANIC("VFIO group %d is not viable", group_id_);
  }
}

void VfioPci::SetupVfioContainer() {
  /* Create container */
  container_fd_ = open("/dev/vfio/vfio", O_RDWR);
  if (container_fd_ < 0) {
    MV_PANIC("failed to open /dev/vfio/vfio");
  }

  /* Here use type1 iommu */
  MV_ASSERT(ioctl(container_fd_, VFIO_GET_API_VERSION) == VFIO_API_VERSION);
  MV_ASSERT(ioctl(container_fd_, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU));
  MV_ASSERT(ioctl(group_fd_, VFIO_GROUP_SET_CONTAINER, &container_fd_) == 0);
  MV_ASSERT(ioctl(container_fd_, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) == 0);
  
  /* Get IOMMU type1 info */
  size_t argsz = sizeof(vfio_iommu_type1_info);
  auto info = (vfio_iommu_type1_info*)malloc(argsz);
  info->argsz = argsz;
  MV_ASSERT(ioctl(container_fd_, VFIO_IOMMU_GET_INFO, info) == 0);
  if (info->argsz != argsz) {
    info = (vfio_iommu_type1_info*)realloc(info, info->argsz);
    MV_ASSERT(ioctl(container_fd_, VFIO_IOMMU_GET_INFO, info) == 0);
  }

  /* Enumerate capabilities, currently migration capability */
  if (info->flags & VFIO_IOMMU_INFO_CAPS && info->cap_offset) {
    uint8_t* ptr = (uint8_t*)info;
    auto cap_header = (vfio_info_cap_header*)(ptr + info->cap_offset);
    while (true) {
      if (cap_header->id == VFIO_IOMMU_TYPE1_INFO_CAP_MIGRATION) {
        auto cap_migration = (vfio_iommu_type1_info_cap_migration*)cap_header;
        /* page size should support 4KB */
        MV_ASSERT(cap_migration->pgsize_bitmap & PAGE_SIZE);
      }
      if (cap_header->next) {
        cap_header = (vfio_info_cap_header*)(ptr + cap_header->next);
      } else {
        break;
      }
    }
  }
  free(info);
}

void VfioPci::SetupVfioDevice() {
  device_name_ = basename(sysfs_path_.c_str());
  device_fd_ = ioctl(group_fd_, VFIO_GROUP_GET_DEVICE_FD, device_name_.c_str());
 
  /* get device info */
  device_info_.argsz = sizeof(device_info_);
  MV_ASSERT(ioctl(device_fd_, VFIO_DEVICE_GET_INFO, &device_info_) == 0);
  
  MV_ASSERT(device_info_.flags & VFIO_DEVICE_FLAGS_RESET);
  MV_ASSERT(device_info_.flags & VFIO_DEVICE_FLAGS_PCI);
  MV_ASSERT(device_info_.num_regions > VFIO_PCI_CONFIG_REGION_INDEX);
  MV_ASSERT(device_info_.num_irqs > VFIO_PCI_MSIX_IRQ_INDEX);

  /* find vfio regions */
  bzero(&regions_, sizeof(regions_));
  for (uint index = VFIO_PCI_BAR0_REGION_INDEX; index < device_info_.num_regions; index++) {
    auto argsz = sizeof(vfio_region_info);
    auto region_info = (vfio_region_info*)malloc(argsz);
    bzero(region_info, argsz);
    region_info->argsz = argsz;
    region_info->index = index;

    MV_ASSERT(ioctl(device_fd_, VFIO_DEVICE_GET_REGION_INFO, region_info) == 0);
    if (region_info->argsz != argsz) {
      region_info = (vfio_region_info*)realloc(region_info, region_info->argsz);
      MV_ASSERT(ioctl(device_fd_, VFIO_DEVICE_GET_REGION_INFO, region_info) == 0);
    }
    if (!region_info->size)
      continue;
    if (region_info->index >= MAX_VFIO_REGIONS)
      continue;
    
    auto &region = regions_[region_info->index];
    region.index = region_info->index;
    region.flags = region_info->flags;
    region.offset = region_info->offset;
    region.size = region_info->size;

    if ((region_info->flags & VFIO_REGION_INFO_FLAG_CAPS) && region_info->cap_offset) {
      auto ptr = (uint8_t*)region_info;
      auto cap_header = (vfio_info_cap_header*)(ptr + region_info->cap_offset);
      while (true) {
        if (cap_header->id == VFIO_REGION_INFO_CAP_SPARSE_MMAP) {
          auto cap_mmap = (vfio_region_info_cap_sparse_mmap*)cap_header;
          for (uint j = 0; j < cap_mmap->nr_areas; j++) {
            region.mmap_areas.push_back({
              .offset = cap_mmap->areas[j].offset,
              .size = cap_mmap->areas[j].size
            });
          }
        } else if (cap_header->id == VFIO_REGION_INFO_CAP_TYPE) {
          auto cap_type = (vfio_region_info_cap_type*)cap_header;
          region.type = cap_type->type;
          region.subtype = cap_type->subtype;
        }
        if (cap_header->next) {
          cap_header = (vfio_info_cap_header*)(ptr + cap_header->next);
        } else {
          break;
        }
      }
    }
    if (debug_) {
      MV_LOG("region index=%u flags=0x%x size=0x%lx type=%d subtype=%d sparse=%lu",
        region.index, region.flags, region.size, region.type, region.subtype, region.mmap_areas.size());
    }
    free(region_info);
  }

  /* find vfio interrupts */
  for (auto &interrupt : interrupts_) {
    interrupt.event_fd = -1;
  }
  for (uint index = 0; index < device_info_.num_irqs; index++) {
    vfio_irq_info irq_info = { .argsz = sizeof(irq_info) };
    irq_info.index = index;
    auto ret = ioctl(device_fd_, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
    if (debug_) {
      MV_LOG("irq index=%d size=%u flags=%x count=%d ret=%d", index,
        irq_info.argsz, irq_info.flags, irq_info.count, ret);
    }
    /* FIXME: currently my mdev only uses one IRQ */
    if (index == VFIO_PCI_MSI_IRQ_INDEX) {
      MV_ASSERT(irq_info.count == 1);
      MV_ASSERT(irq_info.flags & VFIO_IRQ_INFO_EVENTFD);
    }
  }
}

bool VfioPci::MapBarRegion(uint8_t index) {
  auto &bar = pci_bars_[index];
  auto &region = regions_[index];
  int protect = 0;
  if (region.flags & VFIO_REGION_INFO_FLAG_READ)
    protect |= PROT_READ;
  if (region.flags & VFIO_REGION_INFO_FLAG_WRITE)
    protect |= PROT_WRITE;
  if (region.mmap_areas.empty()) {
    bar.host_memory = mmap(nullptr, region.size, protect, MAP_SHARED, device_fd_, region.offset);
    AddIoResource(kIoResourceTypeRam, bar.address, bar.size, bar.host_memory, "vfio-bar-ram");
  } else {
    /* The MMIO region is overlapped by the mmap areas */
    AddIoResource(kIoResourceTypeMmio, bar.address, bar.size, "vfio-bar-mmio");
    for (auto &area : region.mmap_areas) {
      area.mmap = mmap(nullptr, area.size, protect, MAP_SHARED, device_fd_, region.offset + area.offset);
      if (area.mmap == MAP_FAILED) {
        MV_PANIC("failed to map region %d, area offset=0x%lx size=0x%lx", index, area.offset, area.size);
      }
      AddIoResource(kIoResourceTypeRam, bar.address + area.offset, area.size, area.mmap, "vfio-bar-ram");
    }
  }
  return true;
}

bool VfioPci::UnmapBarRegion(uint8_t index) {
  auto &bar = pci_bars_[index];
  auto &region = regions_[index];
  if (region.mmap_areas.empty()) {
    RemoveIoResource(kIoResourceTypeRam, bar.address);
    munmap(bar.host_memory, region.size);
  } else {
    for (auto &area : region.mmap_areas) {
      RemoveIoResource(kIoResourceTypeRam, region.offset + area.offset);
      munmap(area.mmap, area.size);
    }
    RemoveIoResource(kIoResourceTypeMmio, bar.address);
  }
  return true;
}

bool VfioPci::ActivatePciBar(uint8_t index) {
  auto &region = regions_[index];
  if (region.flags & VFIO_REGION_INFO_FLAG_MMAP) {
    return MapBarRegion(index);
  }

  return PciDevice::ActivatePciBar(index);
}

bool VfioPci::DeactivatePciBar(uint8_t index) {
  auto &region = regions_[index];
  if (region.flags & VFIO_REGION_INFO_FLAG_MMAP) {
    return UnmapBarRegion(index);
  }
  return PciDevice::DeactivatePciBar(index);
}


ssize_t VfioPci::ReadRegion(uint8_t index, uint64_t offset, uint8_t* data, uint32_t length) {
  MV_ASSERT(index < MAX_VFIO_REGIONS);
  auto &region = regions_[index];
  return pread(device_fd_, data, length, region.offset + offset);
}

ssize_t VfioPci::WriteRegion(uint8_t index, uint64_t offset, uint8_t* data, uint32_t length) {
  MV_ASSERT(index < MAX_VFIO_REGIONS);
  auto &region = regions_[index];
  return pwrite(device_fd_, data, length, region.offset + offset);
}

void VfioPci::Write(const IoResource& ir, uint64_t offset, uint8_t* data, uint32_t size) {
  for (int i = 0; i < PCI_BAR_NUMS; i++) {
    if (pci_bars_[i].address == ir.base) {
      WriteRegion(i, offset, data, size);
      return;
    }
  }
  PciDevice::Write(ir, offset, data, size);
}

void VfioPci::Read(const IoResource& ir, uint64_t offset, uint8_t* data, uint32_t size) {
  for (int i = 0; i < PCI_BAR_NUMS; i++) {
    if (pci_bars_[i].address == ir.base) {
      ReadRegion(i, offset, data, size);
      return;
    }
  }
  PciDevice::Read(ir, offset, data, size);
}

void VfioPci::SetupPciInterrupts() {
  MV_ASSERT(!msi_config_.is_msix);
  MV_ASSERT(msi_config_.is_64bit);
  uint nr_vectors = 1 << ((msi_config_.msi64->control & PCI_MSI_FLAGS_QSIZE) >> 4);
  MV_ASSERT(nr_vectors == 1);

  /* FIXME: should use irq fd */
  for (uint vector = 0; vector < nr_vectors; vector++) {
    auto &interrupt = interrupts_[vector];
    interrupt.event_fd = eventfd(0, 0);
    MV_ASSERT(interrupt.event_fd != -1);
    manager_->io()->StartPolling(interrupt.event_fd, EPOLLIN, [this, vector](auto events) {
      auto &interrupt = interrupts_[vector];
      uint64_t tmp;
      read(interrupt.event_fd, &tmp, sizeof(tmp));
      SignalMsi(vector);
    });
  }
}

void VfioPci::UpdateMsiRoutes() {
  msi_config_.enabled = msi_config_.msi64->control & PCI_MSI_FLAGS_ENABLE;
  uint nr_vectors = 1 << ((msi_config_.msi64->control & PCI_MSI_FLAGS_QSIZE) >> 4);
  MV_ASSERT(nr_vectors == 1);

  for (uint vector = 0; vector < nr_vectors; vector++) {
    auto &interrupt = interrupts_[vector];
    int event_fd = msi_config_.enabled ? interrupt.event_fd : -1;
    uint8_t buffer[sizeof(vfio_irq_set) + sizeof(int)];
    auto irq_set = (vfio_irq_set*)buffer;
    irq_set->argsz = sizeof(vfio_irq_set) + sizeof(int);
    irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set->index = VFIO_PCI_MSI_IRQ_INDEX;
    irq_set->start = vector;
    irq_set->count = 1;
    memcpy(irq_set->data, &event_fd, sizeof(int));

    auto ret = ioctl(device_fd_, VFIO_DEVICE_SET_IRQS, irq_set);
    if (debug_) {
      MV_LOG("update MSI %d eventfd=%d ret=%d", vector, event_fd, ret);
    }
    if (ret < 0) {
      MV_PANIC("failed to set MSI %u event_fd=%d", vector, event_fd);
    }
  }
}

void VfioPci::WritePciConfigSpace(uint64_t offset, uint8_t* data, uint32_t length) {
  MV_ASSERT(length <= 4);
  MV_ASSERT(offset + length <= PCI_DEVICE_CONFIG_SIZE);
  auto &config_region = regions_[VFIO_PCI_CONFIG_REGION_INDEX];

  /* write the VFIO device, check if msi */
  auto ret = pwrite(device_fd_, data, length, config_region.offset + offset);
  MV_ASSERT(ret == (ssize_t)length);

  /* the default bahavior detects BAR activate/deactivate */
  PciDevice::WritePciConfigSpace(offset, data, length);

  /* update interrupts if needed */
  if (ranges_overlap(offset, length, msi_config_.offset + PCI_MSI_FLAGS, 1)) {
    UpdateMsiRoutes();
  }
}

void VfioPci::ReadPciConfigSpace(uint64_t offset, uint8_t* data, uint32_t length) {
  MV_ASSERT(offset + length <= PCI_DEVICE_CONFIG_SIZE);
  auto &config_region = regions_[VFIO_PCI_CONFIG_REGION_INDEX];

  /* read from VFIO device */
  auto ret = pread(device_fd_, pci_header_.data + offset, length, config_region.offset + offset);
  MV_ASSERT(ret == (ssize_t)length);

  PciDevice::ReadPciConfigSpace(offset, data, length);
}

DECLARE_DEVICE(VfioPci);
