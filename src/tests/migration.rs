use crate::process::CpuModel as Cpu;
use crate::process::{ExpectedOutput, QemuConfig, QemuPayload, QemuProcess};
use anyhow::{Context, Result};
use log::debug;
use qapi::qmp::{self, RunState};
use test_macro::test_fn;

const GUEST_BIN: &[u8] = include_bytes!("../../payload/guest.bin");
const EXPECTED_OUTPUT: &str = "HELLO FROM GUEST";
const KERNEL: &str = "payload/vmlinuz-virt";
const INITRD: &str = "payload/initrd.img";

fn do_migration(
    src: &mut QemuProcess,
    dst: &mut QemuProcess,
    mig_sock: &std::path::Path,
) -> Result<()> {
    dst.qmp()
        .execute(&qmp::migrate_incoming {
            uri: Some(format!("unix:{}", mig_sock.display())),
            channels: None,
            exit_on_error: None,
        })
        .context("dest: migrate_incoming failed")?;
    debug!("destination VM listening for migration");

    src.qmp()
        .execute(&qmp::migrate {
            uri: Some(format!("unix:{}", mig_sock.display())),
            channels: None,
            detach: None,
            resume: None,
        })
        .context("source: migrate failed")?;
    debug!("source VM migration initiated");

    dst.poll_status(RunState::running)?;
    debug!("destination VM running");

    Ok(())
}

#[test_fn]
pub(crate) fn test_live_migration() -> Result<()> {
    let src_dir = tempfile::tempdir().context("failed to create src temp dir")?;
    let dst_dir = tempfile::tempdir().context("failed to create dst temp dir")?;
    let mig_dir = tempfile::tempdir().context("failed to create migration temp dir")?;
    let mig_sock = mig_dir.path().join("migration.sock");

    let guest_bin_path = src_dir.path().join("guest.bin");
    std::fs::write(&guest_bin_path, GUEST_BIN).context("failed to write guest binary")?;
    let payload = QemuPayload::GuestBin(guest_bin_path);

    let cfg = QemuConfig::new(&src_dir, &payload);
    let mut src = QemuProcess::spawn(cfg.clone()).context("failed to spawn source VM")?;

    let cfg = cfg.with_incoming(&dst_dir);
    let mut dst = QemuProcess::spawn(cfg).context("failed to spawn dest VM")?;

    do_migration(&mut src, &mut dst, &mig_sock)?;

    let expected_output = ExpectedOutput::SubString(EXPECTED_OUTPUT.into());
    dst.poll_line(expected_output)
        .context("destination: guest not producing serial output after migration")?;

    Ok(())
}

#[test_fn(
    cpu = {Cpu::Qemu64, Cpu::Host},
    smp = {1, 2, 4},
)]
pub(crate) fn test_live_migration_kernel(cpu: Cpu, smp: u8) -> Result<()> {
    let src_dir = tempfile::tempdir().context("failed to create src temp dir")?;
    let dst_dir = tempfile::tempdir().context("failed to create dst temp dir")?;
    let mig_dir = tempfile::tempdir().context("failed to create migration temp dir")?;
    let mig_sock = mig_dir.path().join("migration.sock");

    let payload = QemuPayload::Kernel {
        kernel: KERNEL.into(),
        initrd: Some(INITRD.into()),
    };

    // Boot source and wait for init to signal it's alive
    let cfg = QemuConfig::new(&src_dir, &payload)
        .with_cpu_model(cpu)
        .with_smp(smp);
    let mut src = QemuProcess::spawn(cfg.clone()).context("failed to spawn source VM")?;
    src.poll_line(ExpectedOutput::SubString("INIT:READY".into()))
        .context("init did not start on source")?;
    debug!("init active on source");

    // Start destination in incoming mode
    let cfg = cfg.with_incoming(&dst_dir);
    let mut dst = QemuProcess::spawn(cfg).context("failed to spawn dest VM")?;

    do_migration(&mut src, &mut dst, &mig_sock)?;

    // Verify init resumed on destination (produces "B" periodically)
    dst.poll_line(ExpectedOutput::SubString("INIT:ALIVE".into()))
        .context("init did not resume on destination after migration")?;
    debug!("init resumed on destination");

    Ok(())
}
