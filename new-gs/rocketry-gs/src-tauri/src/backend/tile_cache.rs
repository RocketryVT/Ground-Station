use serde::Serialize;
use std::{
    fs,
    hash::{Hash, Hasher},
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread::{self, JoinHandle},
    time::{Duration, SystemTime},
};

#[derive(Clone, Serialize)]
pub struct TileCacheInfo {
    pub proxy_url: String,
    pub cache_dir: String,
}

pub struct TileCache {
    info: TileCacheInfo,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl TileCache {
    pub fn start<F>(cache_dir: PathBuf, mut log: F) -> Result<Self, Box<dyn std::error::Error>>
    where
        F: FnMut(String) + Send + 'static,
    {
        fs::create_dir_all(&cache_dir)?;
        let listener = TcpListener::bind("127.0.0.1:0")?;
        listener.set_nonblocking(true)?;
        let port = listener.local_addr()?.port();
        let proxy_url = format!("http://127.0.0.1:{port}/tile-cache?url=");
        let info = TileCacheInfo {
            proxy_url,
            cache_dir: cache_dir.display().to_string(),
        };
        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = stop.clone();
        let thread_cache_dir = cache_dir.clone();

        log(format!(
            "[tile-cache] writing {} via http://127.0.0.1:{port}/tile-cache",
            cache_dir.display()
        ));

        let thread = thread::spawn(move || {
            let client = match reqwest::blocking::Client::builder()
                .timeout(Duration::from_secs(30))
                .user_agent("rocketry-gs-tile-cache/1.0")
                .build()
            {
                Ok(client) => client,
                Err(error) => {
                    log(format!("[tile-cache] HTTP client failed: {error}"));
                    return;
                }
            };

            while !thread_stop.load(Ordering::Relaxed) {
                match listener.accept() {
                    Ok((stream, _)) => {
                        if let Err(error) = handle_connection(stream, &thread_cache_dir, &client) {
                            log(format!("[tile-cache] request failed: {error}"));
                        }
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(20));
                    }
                    Err(error) => {
                        log(format!("[tile-cache] accept failed: {error}"));
                        thread::sleep(Duration::from_millis(100));
                    }
                }
            }
        });

        Ok(Self {
            info,
            stop,
            thread: Some(thread),
        })
    }

    pub fn info(&self) -> TileCacheInfo {
        self.info.clone()
    }

    pub fn stop(&self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

impl Drop for TileCache {
    fn drop(&mut self) {
        self.stop();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn handle_connection(
    mut stream: TcpStream,
    cache_dir: &Path,
    client: &reqwest::blocking::Client,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut buffer = [0u8; 8192];
    let read = stream.read(&mut buffer)?;
    if read == 0 {
        return Ok(());
    }

    let request = String::from_utf8_lossy(&buffer[..read]);
    let Some(request_line) = request.lines().next() else {
        return Ok(());
    };
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or("");
    let target = parts.next().unwrap_or("");

    if method == "OPTIONS" {
        write_empty_response(&mut stream, 204, "No Content")?;
        return Ok(());
    }

    if method != "GET" && method != "HEAD" {
        write_text_response(&mut stream, 405, "Method Not Allowed", "method not allowed")?;
        return Ok(());
    }

    let Some(encoded_url) = target.strip_prefix("/tile-cache?url=") else {
        write_text_response(&mut stream, 404, "Not Found", "not found")?;
        return Ok(());
    };
    let url = percent_decode(encoded_url)?;
    if !url.starts_with("https://") && !url.starts_with("http://") {
        write_text_response(&mut stream, 400, "Bad Request", "unsupported url")?;
        return Ok(());
    }

    let entry = CacheEntry::new(cache_dir, &url)?;
    if !entry.body_path.exists() {
        let _ = fetch_to_cache(client, &url, &entry);
    }

    if entry.body_path.exists() {
        let body = if method == "HEAD" {
            Vec::new()
        } else {
            fs::read(&entry.body_path)?
        };
        let content_type = fs::read_to_string(&entry.meta_path)
            .ok()
            .and_then(|meta| read_meta_value(&meta, "content-type"))
            .unwrap_or_else(|| "application/octet-stream".to_string());
        write_binary_response(&mut stream, 200, "OK", &content_type, &body)?;
    } else {
        write_text_response(&mut stream, 504, "Gateway Timeout", "tile not cached")?;
    }

    Ok(())
}

struct CacheEntry {
    body_path: PathBuf,
    meta_path: PathBuf,
}

impl CacheEntry {
    fn new(cache_dir: &Path, url: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let host = host_from_url(url).unwrap_or("unknown").replace(':', "_");
        let dir = cache_dir.join(host);
        fs::create_dir_all(&dir)?;
        let key = stable_hash(url);
        Ok(Self {
            body_path: dir.join(format!("{key:016x}.bin")),
            meta_path: dir.join(format!("{key:016x}.meta")),
        })
    }
}

fn fetch_to_cache(
    client: &reqwest::blocking::Client,
    url: &str,
    entry: &CacheEntry,
) -> Result<(), Box<dyn std::error::Error>> {
    let response = client.get(url).send()?;
    let status = response.status();
    if !status.is_success() {
        return Err(format!("upstream {status} for {url}").into());
    }

    let content_type = response
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
        .unwrap_or("application/octet-stream")
        .to_string();
    let bytes = response.bytes()?;

    let tmp_path = entry.body_path.with_extension("tmp");
    fs::write(&tmp_path, &bytes)?;
    fs::rename(tmp_path, &entry.body_path)?;
    fs::write(
        &entry.meta_path,
        format!(
            "url={url}\ncontent-type={content_type}\nfetched-at={:?}\n",
            SystemTime::now()
        ),
    )?;
    Ok(())
}

fn write_binary_response(
    stream: &mut TcpStream,
    status: u16,
    reason: &str,
    content_type: &str,
    body: &[u8],
) -> std::io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Length: {}\r\nContent-Type: {content_type}\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        body.len()
    )?;
    stream.write_all(body)
}

fn write_text_response(
    stream: &mut TcpStream,
    status: u16,
    reason: &str,
    body: &str,
) -> std::io::Result<()> {
    write_binary_response(
        stream,
        status,
        reason,
        "text/plain; charset=utf-8",
        body.as_bytes(),
    )
}

fn write_empty_response(stream: &mut TcpStream, status: u16, reason: &str) -> std::io::Result<()> {
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Length: 0\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nConnection: close\r\n\r\n"
    )
}

fn read_meta_value(meta: &str, key: &str) -> Option<String> {
    meta.lines().find_map(|line| {
        line.strip_prefix(&format!("{key}="))
            .map(ToString::to_string)
    })
}

fn host_from_url(url: &str) -> Option<&str> {
    let rest = url
        .strip_prefix("https://")
        .or_else(|| url.strip_prefix("http://"))?;
    Some(
        rest.split('/')
            .next()?
            .split('@')
            .next_back()?
            .split('?')
            .next()?,
    )
}

fn stable_hash(input: &str) -> u64 {
    let mut hasher = Fnv1a64::default();
    input.hash(&mut hasher);
    hasher.finish()
}

#[derive(Default)]
struct Fnv1a64(u64);

impl Hasher for Fnv1a64 {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        if self.0 == 0 {
            self.0 = 0xcbf29ce484222325;
        }
        for byte in bytes {
            self.0 ^= u64::from(*byte);
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
}

fn percent_decode(input: &str) -> Result<String, Box<dyn std::error::Error>> {
    let mut output = Vec::with_capacity(input.len());
    let bytes = input.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' if i + 2 < bytes.len() => {
                let high = hex_value(bytes[i + 1])?;
                let low = hex_value(bytes[i + 2])?;
                output.push((high << 4) | low);
                i += 3;
            }
            b'+' => {
                output.push(b' ');
                i += 1;
            }
            byte => {
                output.push(byte);
                i += 1;
            }
        }
    }
    Ok(String::from_utf8(output)?)
}

fn hex_value(byte: u8) -> Result<u8, Box<dyn std::error::Error>> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => Err("invalid percent encoding".into()),
    }
}
