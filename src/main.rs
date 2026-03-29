use anyhow::{Context, Result};
use process::{QemuPayload, QemuProcess};
use qapi::qmp;

mod process;

const GUEST_BIN: &[u8] = include_bytes!("../payload/guest.bin");
const KERNEL: &str = "payload/vmlinuz-virt";
const EXPECTED_OUTPUT: &str = "HELLO FROM GUEST";

macro_rules! function_name {
    () => {{
        fn f() {}
        let name = std::any::type_name_of_val(&f);
        name.rsplit("::").nth(1).unwrap()
    }};
}

fn test_simple_guest_bin() -> Result<()> {
    println!("--- {} ---", function_name!());
    let tmp_dir = tempfile::tempdir().context("failed to create temp dir")?;
    let guest_bin_path = tmp_dir.path().join("guest.bin");
    std::fs::write(&guest_bin_path, GUEST_BIN).context("failed to write guest binary")?;
    let payload = QemuPayload::GuestBin(guest_bin_path.into());
    let mut process =
        QemuProcess::spawn(&tmp_dir, &payload).context("failed to spawn QEMU process")?;

    let status = process
        .qmp()
        .execute(&qmp::query_status {})
        .context("query_status failed")?;
    println!("VM status: {:?}", status.status);

    process
        .wait_for_line(EXPECTED_OUTPUT)
        .context("expected output not found")?;
    println!("✓ guest serial output verified!");

    let _ = process.qmp().execute(&qmp::quit {});
    let exit = process.wait().context("failed to wait for QEMU")?;
    println!("QEMU exited: {exit}");

    Ok(())
}

fn test_kernel_boot() -> Result<()> {
    println!("--- {} ---", function_name!());
    let tmp_dir = tempfile::tempdir().context("failed to create temp dir")?;
    let payload = QemuPayload::Kernel(KERNEL.into());
    let mut process =
        QemuProcess::spawn(&tmp_dir, &payload).context("failed to spawn QEMU process")?;

    let status = process
        .qmp()
        .execute(&qmp::query_status {})
        .context("query_status failed")?;
    println!("VM status: {:?}", status.status);

    process
        .wait_for_line("Hypervisor detected")
        .context("kernel boot output not found")?;
    println!("✓ kernel boot verified!");

    let _ = process.qmp().execute(&qmp::quit {});
    let exit = process.wait().context("failed to wait for QEMU")?;
    println!("QEMU exited: {exit}");

    Ok(())
}

fn main() -> Result<()> {
    test_simple_guest_bin()?;
    test_kernel_boot()?;

    Ok(())
}
