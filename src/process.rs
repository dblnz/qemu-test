use anyhow::{bail, Context, Result};
use qapi::{Qmp, Stream};
use std::io::{BufRead, BufReader};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Child, Command};
use std::thread;
use std::time::{Duration, Instant};
use tempfile::TempDir;

const TIMEOUT: Duration = Duration::from_secs(10);

enum Accelerator {
    Kvm,
}

enum Machine {
    Pc,
}

struct Unconnected {
    path: PathBuf,
}

struct Connected {
    stream: UnixStream,
}

struct Socket<State> {
    state: State,
}

impl Socket<Unconnected> {
    pub fn new(path: PathBuf) -> Self {
        Self {
            state: Unconnected { path },
        }
    }

    pub fn connect(self, timeout: Duration) -> Result<Socket<Connected>> {
        let path = self.state.path;
        let start = Instant::now();
        while !path.exists() {
            if start.elapsed() > timeout {
                bail!("timeout waiting for socket: {}", path.display());
            }
            thread::sleep(Duration::from_millis(50));
        }
        let stream = UnixStream::connect(&path).context("failed to connect to QMP socket")?;

        let socket = Socket {
            state: Connected { stream },
        };

        Ok(socket)
    }
}

struct GuestConfig {
    ram_mb: u16,
    qmp_sock_path: PathBuf,
    serial_sock_path: PathBuf,
    accel: Accelerator,
    machine: Machine,
    payload: Option<QemuPayload>,
}

impl From<&GuestConfig> for Vec<String> {
    fn from(cfg: &GuestConfig) -> Self {
        let mut args = vec![
            "-display".into(),
            "none".into(),
            "-no-reboot".into(),
            "-cpu".into(),
            "host".into(),
        ];

        args.extend([
            "-qmp".into(),
            format!("unix:{},server=on,wait=off", cfg.qmp_sock_path.display()),
        ]);

        args.extend([
            "-serial".into(),
            format!("unix:{},server=on,wait=off", cfg.serial_sock_path.display()),
        ]);

        args.extend([
            "-accel".into(),
            match cfg.accel {
                Accelerator::Kvm => "kvm".into(),
            },
        ]);

        args.extend([
            "-M".into(),
            match cfg.machine {
                Machine::Pc => "pc".into(),
            },
        ]);

        if let Some(payload) = &cfg.payload {
            match payload {
                QemuPayload::GuestBin(path) => {
                    args.push("-drive".into());
                    args.push(format!("format=raw,file={},if=floppy", path.display()));
                }
            }
        }

        args.extend(["-m".into(), format!("{}m", cfg.ram_mb)]);

        args
    }
}

#[derive(Clone)]
pub(crate) enum QemuPayload {
    GuestBin(PathBuf),
}

pub(crate) struct QemuProcess {
    child: Child,
    qmp: Qmp<Stream<BufReader<UnixStream>, UnixStream>>,
    serial_reader: BufReader<UnixStream>,
}

impl QemuProcess {
    pub fn spawn(tmp_dir: &TempDir, payload: &QemuPayload) -> Result<Self> {
        let qmp_sock_path = tmp_dir.path().join("qmp.sock");
        let serial_sock_path = tmp_dir.path().join("serial.sock");

        let cfg = GuestConfig {
            ram_mb: 32,
            serial_sock_path: serial_sock_path.clone(),
            qmp_sock_path: qmp_sock_path.clone(),
            payload: Some(payload.clone()),
            accel: Accelerator::Kvm,
            machine: Machine::Pc,
        };

        let args: Vec<String> = (&cfg).into();
        let child = Command::new("qemu-system-x86_64")
            .args(args)
            .spawn()
            .context("failed to start qemu-system-x86_64")?;
        println!("QEMU started (pid {})", child.id());

        let qmp_sock = Socket::new(qmp_sock_path);
        let stream = qmp_sock.connect(TIMEOUT)?.state.stream;
        let mut qmp = Qmp::new(qapi::Stream::new(
            BufReader::new(stream.try_clone().context("failed to clone stream")?),
            stream,
        ));
        qmp.handshake().context("QMP handshake failed")?;

        let serial_sock = Socket::new(serial_sock_path);
        let stream = serial_sock.connect(TIMEOUT)?.state.stream;
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .context("failed to set serial read timeout")?;
        let serial_reader = BufReader::new(stream);

        let process = Self {
            child,
            qmp,
            serial_reader,
        };
        Ok(process)
    }

    pub fn wait(&mut self) -> std::io::Result<std::process::ExitStatus> {
        self.child.wait()
    }

    pub fn qmp(&mut self) -> &mut Qmp<Stream<BufReader<UnixStream>, UnixStream>> {
        &mut self.qmp
    }

    pub fn wait_for_line(&mut self, expected: &str) -> Result<()> {
        let mut output = String::new();
        let start = Instant::now();

        loop {
            if start.elapsed() > TIMEOUT {
                bail!("timeout waiting for {expected}");
            }

            let mut line = String::new();
            match self.serial_reader.read_line(&mut line) {
                Ok(0) => bail!("connection closed while waiting for {expected}"),
                Ok(_) => {
                    print!("[serial] {line}");
                    output.push_str(&line);
                    if output.contains(expected) {
                        return Ok(());
                    }
                }
                Err(e)
                    if e.kind() == std::io::ErrorKind::WouldBlock
                        || e.kind() == std::io::ErrorKind::TimedOut =>
                {
                    continue;
                }
                Err(e) => bail!("serial read error: {e}"),
            }
        }
    }
}
