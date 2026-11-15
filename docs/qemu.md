# Reproducing the VFIO path on Linux

The macOS build host used for the measurements in this repository cannot run
VFIO.  `src/backend_vfio.cpp` is therefore **compile-verified only** here; it is
built by CI on `ubuntu-latest` with `-DNVMESS_BACKEND_VFIO=ON`, but no number in
`results/` comes from it.  This guide is the exact procedure for a Linux host.

## 1. Build with the VFIO backend

```bash
cmake -S . -B build-vfio -DNVMESS_BACKEND_VFIO=ON
cmake --build build-vfio -j
```

This adds `src/backend_vfio.cpp` and the `vfio-probe` subcommand.  The queue
layer still binds to the in-process controller (`NvmeDriver` throws if the
backend is not simulated), so `vfio-probe` validates the transport only:
opening the container and group, setting up the type-1 IOMMU, mapping BAR0 and
decoding CAP/VS/CSTS.

## 2. Bind the controller to vfio-pci (inside the Linux guest or on bare metal)

```bash
# 1. Find the NVMe controller and its IOMMU group.
lspci -nn | grep -i "non-volatile memory"
BDF=0000:01:00.0
GROUP=$(basename "$(readlink /sys/bus/pci/devices/$BDF/iommu_group)")
echo "IOMMU group: $GROUP"

# 2. IOMMU must be on: add to the kernel command line and reboot
#    Intel:  intel_iommu=on iommu=pt
#    AMD:    amd_iommu=on iommu=pt

# 3. Detach the kernel driver and bind vfio-pci.
echo "$BDF" > /sys/bus/pci/devices/$BDF/driver/unbind
echo vfio-pci > /sys/bus/pci/devices/$BDF/driver_override
echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind

# 4. Reserve hugepages for the DMA arena (optional; the backend falls back
#    to 4 KiB anonymous memory).
echo 512 > /proc/sys/vm/nr_hugepages

# 5. Probe BAR0 through VFIO.
./build-vfio/nvmeslotswap vfio-probe --vfio-device "$BDF" --vfio-group /dev/vfio/$GROUP
```

Expected shape of the output:

```
backend: vfio
BAR0 mapped, doorbell stride 4 bytes
CAP=0x... (MQES=1024)  VS=0x00010400  CSTS=0x00000000
```

The group must contain only devices that are all bound to vfio-pci, otherwise
`VFIO_GROUP_GET_STATUS` reports it as not viable.

## 3. QEMU-emulated NVMe controller

QEMU's emulated NVMe controller is visible to a Linux guest as a normal PCI
device, so the same bind procedure works inside the guest.  Boot the guest
with:

```bash
truncate -s 8G nvme.img
qemu-system-x86_64 \
  -machine q35,accel=kvm \
  -cpu host -smp 4 -m 8G \
  -kernel /boot/vmlinuz \
  -append "root=/dev/vda1 console=ttyS0 intel_iommu=on iommu=pt" \
  -drive file=rootfs.qcow2,if=virtio,format=qcow2 \
  -drive file=nvme.img,if=none,id=nvmedisk,format=raw \
  -device nvme,drive=nvmedisk,serial=NVMESS0001,id=nvme0 \
  -nographic
```

Then, inside the guest:

```bash
lspci -nn | grep -i "QEMU NVM Express"
# e.g. 00:04.0 Non-Volatile memory controller [0108]: Red Hat, Inc. QEMU NVM Express Controller [1b36:0010]
```

Bind that BDF to `vfio-pci` exactly as in step 2 and run `vfio-probe`.

### What QEMU's controller does not implement

QEMU's NVMe model implements Identify, queue creation, PRPs and read/write, but
not Firmware Image Download (0x11), Firmware Commit (0x10) or the Firmware Slot
Information log page (0x03).  The VFIO path can therefore validate transport,
doorbells, queue and PRP plumbing against QEMU, but the A/B activation story in
this repository is demonstrated against the in-repo emulated controller (and
would work against a real SSD that implements those admin commands).
