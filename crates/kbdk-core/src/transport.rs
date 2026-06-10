use anyhow::Result;
use std::path::Path;

#[derive(Debug, Clone)]
pub struct ExecResult {
    pub output: String,
    pub rc: i32,
}

pub trait Transport {
    fn exec(&self, cmd: &str, timeout_secs: u64) -> Result<ExecResult>;
    /// Verified upload: size + md5 checked on the board, retried on mismatch.
    fn push(&self, local: &Path, remote: &str) -> Result<()>;
    fn pull(&self, remote: &str, local: &Path) -> Result<()>;
    fn name(&self) -> &'static str;
}
