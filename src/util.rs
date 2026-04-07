use rand::RngExt;

fn generate_mac() -> String {
    let mut rng = rand::rng();
    let b: [u8; 3] = rng.random();
    format!("52:54:00:{:02x}:{:02x}:{:02x}", b[0], b[1], b[2])
}

#[derive(Clone)]
pub(crate) struct SshConfig {
    mac: String,
}

impl SshConfig {
    pub fn new() -> Self {
        let mac = generate_mac();
        Self { mac }
    }

    pub fn mac(&self) -> &str {
        &self.mac
    }
}
