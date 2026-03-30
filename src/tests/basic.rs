use crate::process::{QemuConfig, QemuPayload, QemuProcess};
use anyhow::{Context, Result};
use log::debug;
use qapi::qmp;
use test_macro::test_fn;

const GUEST_BIN: &[u8] = include_bytes!("../../payload/guest.bin");
const KERNEL: &str = "payload/vmlinuz-virt";
const EXPECTED_OUTPUT: &str = "HELLO FROM GUEST";

#[test_fn]
pub(crate) fn test_simple_guest_bin() -> Result<()> {
    let tmp_dir = tempfile::tempdir().context("failed to create temp dir")?;
    let guest_bin_path = tmp_dir.path().join("guest.bin");
    std::fs::write(&guest_bin_path, GUEST_BIN).context("failed to write guest binary")?;
    let payload = QemuPayload::GuestBin(guest_bin_path);
    let cfg = QemuConfig::new(&tmp_dir, &payload);
    let mut process = QemuProcess::spawn(cfg).context("failed to spawn QEMU process")?;

    let status = process
        .qmp()
        .execute(&qmp::query_status {})
        .context("query_status failed")?;
    debug!("VM status: {:?}", status.status);

    process
        .poll_line(EXPECTED_OUTPUT)
        .context("expected output not found")?;

    Ok(())
}

#[test_fn]
pub(crate) fn test_kernel_boot() -> Result<()> {
    let tmp_dir = tempfile::tempdir().context("failed to create temp dir")?;
    let payload = QemuPayload::Kernel(KERNEL.into());
    let cfg = QemuConfig::new(&tmp_dir, &payload);
    let mut process = QemuProcess::spawn(cfg).context("failed to spawn QEMU process")?;

    let status = process
        .qmp()
        .execute(&qmp::query_status {})
        .context("query_status failed")?;
    debug!("VM status: {:?}", status.status);

    process
        .poll_line("Hypervisor detected")
        .context("kernel boot output not found")?;

    Ok(())
}
