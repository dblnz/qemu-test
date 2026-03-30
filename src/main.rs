use anyhow::{Result, bail};
use linkme::distributed_slice;
use rayon::prelude::*;
use std::env;

mod process;
mod tests;

#[distributed_slice]
pub static TESTS: [fn() -> Result<()>];

fn main() -> Result<()> {
    env_logger::init();

    let parallelism = env::var("TEST_JOBS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(1);

    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(parallelism)
        .build()
        .expect("failed to build thread pool");

    let errors: Vec<_> =
        pool.install(|| TESTS.par_iter().filter_map(|test| test().err()).collect());

    if !errors.is_empty() {
        for e in &errors {
            eprintln!("{e:?}");
        }
        bail!("FAIL: {} of {} tests failed", errors.len(), TESTS.len());
    }

    println!("\nPASS: All {} tests passed", TESTS.len());
    Ok(())
}
