use anyhow::Result;

mod process;
mod tests;

fn main() -> Result<()> {
    env_logger::init();

    tests::basic::test_simple_guest_bin()?;
    tests::basic::test_kernel_boot()?;
    tests::migration::test_live_migration()?;

    Ok(())
}
