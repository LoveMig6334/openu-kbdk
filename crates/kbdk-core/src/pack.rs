//! Model pack: a directory with manifest.json + ncnn .param/.bin + labels.txt.
//! Schema note: blob keys are named `in_blob`/`out_blob` and the labels file key
//! `labels_file` so the board's flat-JSON parser (board/runner/manifest.h) can
//! find them by unique key — `"input"` already names the input-spec object and
//! `"labels"` the label array.

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Serialize, Deserialize)]
pub struct InputSpec {
    pub width: u32,
    pub height: u32,
    pub mean: [f32; 3],
    pub norm: [f32; 3],
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Blobs {
    pub in_blob: String,
    pub out_blob: String,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct PackFiles {
    pub param: String,
    pub bin: String,
    pub labels_file: String,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Manifest {
    pub name: String,
    pub task: String, // "classification" | "detection"
    pub backbone: String,
    pub input: InputSpec,
    pub quant: String, // "int8" | "fp16"
    pub blobs: Blobs,
    pub files: PackFiles,
    pub md5: std::collections::HashMap<String, String>,
    pub labels: Vec<String>,
}

impl Manifest {
    pub fn load(dir: &Path) -> Result<Self> {
        let p = dir.join("manifest.json");
        let m: Manifest = serde_json::from_str(
            &std::fs::read_to_string(&p).with_context(|| format!("read {}", p.display()))?,
        )?;
        Ok(m)
    }

    /// verify the referenced files exist and md5s match
    pub fn verify(&self, dir: &Path) -> Result<()> {
        for (key, rel) in [("param", &self.files.param), ("bin", &self.files.bin)] {
            let data = std::fs::read(dir.join(rel))
                .with_context(|| format!("pack file missing: {rel}"))?;
            let got = format!("{:x}", md5::compute(&data));
            match self.md5.get(key) {
                Some(want) if *want == got => {}
                Some(want) => bail!("{key} md5 mismatch: {got} != {want}"),
                None => bail!("manifest md5 missing entry {key}"),
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_roundtrip() {
        let j = r#"{
          "name":"toy3","task":"classification","backbone":"mobilenet_v2",
          "input":{"width":224,"height":224,"mean":[127.5,127.5,127.5],"norm":[0.0078125,0.0078125,0.0078125]},
          "quant":"int8",
          "blobs":{"in_blob":"in0","out_blob":"out0"},
          "files":{"param":"model.param","bin":"model.bin","labels_file":"labels.txt"},
          "md5":{"param":"aa","bin":"bb"},
          "labels":["red","green","blue"]
        }"#;
        let m: Manifest = serde_json::from_str(j).unwrap();
        assert_eq!(m.input.width, 224);
        assert_eq!(m.labels.len(), 3);
        assert_eq!(m.blobs.out_blob, "out0");
    }
}
