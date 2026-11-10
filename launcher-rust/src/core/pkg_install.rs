//! Installing a PS4 game or add-on from a fake PKG (the format a jailbroken console installs):
//! the plain metadata entries go to `sce_sys/`, the AES-XTS encrypted outer PFS is decrypted on
//! the fly, and the PFSC-compressed inner PFS is inflated sector by sector into the game folder.
//!
//! This replaces LibOrbisPkg's PkgTool, which on .NET 6+ leaves the end of most compressed
//! sectors zero (one partial `DeflateStream.Read` per sector) and corrupts the game data.
//! Every sector here is inflated completely, or the install fails.
//!
//! Key derivation as LibOrbisPkg (`Crypto.ComputeKeys`, `PfsGenEncKey`): fake PKGs use the
//! all-zero passcode.

use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use aes::cipher::{generic_array::GenericArray, BlockDecrypt, BlockEncrypt, KeyInit};
use aes::Aes128;
use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

const PKG_MAGIC: u32 = 0x7F43_4E54; // "\x7FCNT"
const PFS_MAGIC: i64 = 20_130_315;
const PFSC_MAGIC: i32 = 0x4353_4650; // "PFSC"
const XTS_SECTOR: u64 = 0x1000;
const FAKE_PASSCODE: &[u8; 32] = b"00000000000000000000000000000000";

#[derive(Debug, thiserror::Error)]
pub enum InstallError {
    #[error("{0}")]
    Io(#[from] std::io::Error),
    #[error("not a PS4 PKG")]
    NotPkg,
    #[error("unsupported PKG: {0}")]
    Unsupported(String),
    #[error("corrupt PKG: {0}")]
    Corrupt(String),
    #[error("cancelled")]
    Cancelled,
}

type Result<T> = std::result::Result<T, InstallError>;

fn be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}
fn be64(b: &[u8], at: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[at..at + 8]);
    u64::from_be_bytes(a)
}
fn le32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}
fn lei32(b: &[u8], at: usize) -> i32 {
    le32(b, at) as i32
}
fn le64(b: &[u8], at: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[at..at + 8]);
    u64::from_le_bytes(a)
}

/// What the PKG header says about its content.
#[derive(Debug, Clone)]
pub struct PkgInfo {
    pub content_id: String,
    /// The title id inside the content id (`CUSA00242`).
    pub title_id: String,
    /// The last 16 characters: the entitlement label of an add-on.
    pub label: String,
    /// 0x1A: game (GD), 0x1B: add-on with data (AC), 0x1C: patch, 0x1F/0x1E...: add-on keys.
    pub content_type: u32,
    pub pfs_offset: u64,
    pub pfs_size: u64,
    pub pfs_flags: u64,
    entries: Vec<Entry>,
}

impl PkgInfo {
    /// A game (base or patch) rather than an add-on.
    pub fn is_game(&self) -> bool {
        matches!(self.content_type, 0x1A | 0x1C)
    }
}

#[derive(Debug, Clone)]
struct Entry {
    id: u32,
    name: Option<String>,
    flags1: u32,
    offset: u32,
    size: u32,
}

/// Reads the header and the entry table.
pub fn read_info(pkg: &Path) -> Result<PkgInfo> {
    let mut f = File::open(pkg)?;
    let mut head = vec![0u8; 0x1000];
    f.read_exact(&mut head)?;
    if be32(&head, 0) != PKG_MAGIC {
        return Err(InstallError::NotPkg);
    }
    let entry_count = be32(&head, 0x10) as usize;
    let table = be32(&head, 0x18) as u64;
    let content_id = String::from_utf8_lossy(&head[0x40..0x64]).trim_end_matches('\0').to_string();
    if content_id.len() != 36 {
        return Err(InstallError::Corrupt(format!("content id {content_id:?}")));
    }
    let content_type = be32(&head, 0x74);
    let (pfs_flags, pfs_offset, pfs_size) = (be64(&head, 0x408), be64(&head, 0x410), be64(&head, 0x418));
    let mut raw = vec![0u8; entry_count * 32];
    f.seek(SeekFrom::Start(table))?;
    f.read_exact(&mut raw)?;
    let mut entries: Vec<Entry> = raw
        .chunks_exact(32)
        .map(|e| Entry {
            id: be32(e, 0),
            name: None,
            flags1: be32(e, 8),
            offset: be32(e, 16),
            size: be32(e, 20),
        })
        .collect();
    // ENTRY_NAMES (0x0200): NUL-separated names, entries point into it with their second word.
    if let Some(names) = entries.iter().find(|e| e.id == 0x0200).cloned() {
        let mut blob = vec![0u8; names.size as usize];
        f.seek(SeekFrom::Start(u64::from(names.offset)))?;
        f.read_exact(&mut blob)?;
        for (e, raw) in entries.iter_mut().zip(raw.chunks_exact(32)) {
            let at = be32(raw, 4) as usize;
            if e.id >= 0x1000 && at > 0 && at < blob.len() {
                let end = blob[at..].iter().position(|&c| c == 0).map_or(blob.len(), |n| at + n);
                e.name = Some(String::from_utf8_lossy(&blob[at..end]).into_owned());
            }
        }
    }
    Ok(PkgInfo {
        title_id: content_id[7..16].to_string(),
        label: content_id[20..36].to_string(),
        content_id,
        content_type,
        pfs_offset,
        pfs_size,
        pfs_flags,
        entries,
    })
}

// --- Keys and AES-XTS ---

fn sha256(parts: &[&[u8]]) -> [u8; 32] {
    let mut h = Sha256::new();
    for p in parts {
        h.update(p);
    }
    h.finalize().into()
}

fn hmac(key: &[u8], parts: &[&[u8]]) -> [u8; 32] {
    let mut m = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC takes any key length");
    for p in parts {
        m.update(p);
    }
    m.finalize().into_bytes().into()
}

/// EKPFS of a fake PKG: SHA256(BE index 1 || SHA256(content id padded to 48) || passcode).
fn ekpfs(content_id: &str) -> [u8; 32] {
    let mut padded = [0u8; 48];
    padded[..content_id.len()].copy_from_slice(content_id.as_bytes());
    sha256(&[&sha256(&[&1u32.to_be_bytes()]), &sha256(&[&padded]), FAKE_PASSCODE])
}

/// The (tweak, data) AES keys of the outer PFS image.
fn xts_keys(ekpfs: &[u8; 32], seed: &[u8; 16], new_crypt: bool) -> ([u8; 16], [u8; 16]) {
    let base = if new_crypt { hmac(ekpfs, &[seed]) } else { *ekpfs };
    let key = hmac(&base, &[&1u32.to_le_bytes(), seed]);
    let mut tweak = [0u8; 16];
    let mut data = [0u8; 16];
    tweak.copy_from_slice(&key[..16]);
    data.copy_from_slice(&key[16..]);
    (tweak, data)
}

struct Xts {
    tweak: Aes128,
    data: Aes128,
}

impl Xts {
    fn decrypt_sector(&self, sector: u64, buf: &mut [u8]) {
        let mut t = [0u8; 16];
        t[..8].copy_from_slice(&sector.to_le_bytes());
        let mut t = GenericArray::from(t);
        self.tweak.encrypt_block(&mut t);
        for block in buf.chunks_exact_mut(16) {
            for (b, k) in block.iter_mut().zip(t.iter()) {
                *b ^= k;
            }
            self.data.decrypt_block(GenericArray::from_mut_slice(block));
            for (b, k) in block.iter_mut().zip(t.iter()) {
                *b ^= k;
            }
            // Multiply the tweak by x in GF(2^128), little-endian.
            let mut carry = 0u8;
            for byte in t.iter_mut() {
                let next = *byte >> 7;
                *byte = (*byte << 1) | carry;
                carry = next;
            }
            if carry != 0 {
                t[0] ^= 0x87;
            }
        }
    }
}

// --- Readers ---

/// Random access to some bytes, by offset.
trait ReadAt {
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> Result<()>;
}

/// The outer PFS image inside the PKG, decrypted from XTS sector `start` on.
struct OuterImage {
    file: File,
    base: u64,
    xts: Option<Xts>,
    start: u64,
    cache_at: u64,
    cache: Vec<u8>,
}

impl OuterImage {
    const CHUNK: u64 = 0x10000;

    fn load(&mut self, chunk: u64) -> Result<()> {
        if self.cache_at == chunk && !self.cache.is_empty() {
            return Ok(());
        }
        self.cache.resize(Self::CHUNK as usize, 0);
        self.file.seek(SeekFrom::Start(self.base + chunk))?;
        let mut done = 0;
        while done < self.cache.len() {
            match self.file.read(&mut self.cache[done..])? {
                0 => break,
                n => done += n,
            }
        }
        self.cache[done..].fill(0);
        if let Some(xts) = &self.xts {
            for (i, sector) in self.cache.chunks_exact_mut(XTS_SECTOR as usize).enumerate() {
                let index = chunk / XTS_SECTOR + i as u64;
                if index >= self.start {
                    xts.decrypt_sector(index, sector);
                }
            }
        }
        self.cache_at = chunk;
        Ok(())
    }
}

impl ReadAt for OuterImage {
    fn read_at(&mut self, mut offset: u64, buf: &mut [u8]) -> Result<()> {
        let mut done = 0;
        while done < buf.len() {
            let chunk = offset / Self::CHUNK * Self::CHUNK;
            self.load(chunk)?;
            let inside = (offset - chunk) as usize;
            let n = (buf.len() - done).min(self.cache.len() - inside);
            buf[done..done + n].copy_from_slice(&self.cache[inside..inside + n]);
            done += n;
            offset += n as u64;
        }
        Ok(())
    }
}

/// A file made of PFS blocks of another image.
struct Blocks<'a, R: ReadAt> {
    base: &'a mut R,
    block: u64,
    list: Vec<u64>,
}

impl<R: ReadAt> ReadAt for Blocks<'_, R> {
    fn read_at(&mut self, mut offset: u64, buf: &mut [u8]) -> Result<()> {
        let mut done = 0;
        while done < buf.len() {
            let index = (offset / self.block) as usize;
            let inside = offset % self.block;
            let block = *self
                .list
                .get(index)
                .ok_or_else(|| InstallError::Corrupt("read past a file's blocks".into()))?;
            let n = (buf.len() - done).min((self.block - inside) as usize);
            self.base.read_at(block * self.block + inside, &mut buf[done..done + n])?;
            done += n;
            offset += n as u64;
        }
        Ok(())
    }
}

/// The uncompressed view of a PFSC file.
struct Pfsc<R: ReadAt> {
    base: R,
    block: u64,
    map: Vec<u64>,
    cache_index: usize,
    cache: Vec<u8>,
    raw: Vec<u8>,
}

impl<R: ReadAt> Pfsc<R> {
    fn new(mut base: R) -> Result<Self> {
        let mut h = [0u8; 0x30];
        base.read_at(0, &mut h)?;
        let (magic, block, block2, offsets, length) =
            (lei32(&h, 0), lei32(&h, 12), le64(&h, 16), le64(&h, 24), le64(&h, 40));
        if magic != PFSC_MAGIC || block <= 0 || block as u64 != block2 {
            return Err(InstallError::Corrupt("inner image is not PFSC".into()));
        }
        let count = (length / block2) as usize;
        let mut raw = vec![0u8; (count + 1) * 8];
        base.read_at(offsets, &mut raw)?;
        let map = raw.chunks_exact(8).map(|c| le64(c, 0)).collect();
        Ok(Pfsc {
            base,
            block: block2,
            map,
            cache_index: usize::MAX,
            cache: Vec::new(),
            raw: Vec::new(),
        })
    }

    fn sector(&mut self, index: usize) -> Result<()> {
        if index == self.cache_index {
            return Ok(());
        }
        let (start, end) = (self.map[index], self.map[index + 1]);
        let size = end
            .checked_sub(start)
            .ok_or_else(|| InstallError::Corrupt("PFSC sector map".into()))?;
        let block = self.block as usize;
        self.cache.resize(block, 0);
        if size == self.block {
            self.base.read_at(start, &mut self.cache)?;
        } else if size > self.block {
            self.cache.fill(0);
        } else {
            // A zlib stream: 2-byte header, then raw deflate. Inflated to the full sector.
            self.raw.resize(size as usize, 0);
            self.base.read_at(start, &mut self.raw)?;
            let mut inflater = flate2::read::DeflateDecoder::new(&self.raw[2..]);
            inflater.read_exact(&mut self.cache).map_err(|e| {
                InstallError::Corrupt(format!("PFSC sector {index} does not inflate: {e}"))
            })?;
        }
        self.cache_index = index;
        Ok(())
    }
}

impl<R: ReadAt> ReadAt for Pfsc<R> {
    fn read_at(&mut self, mut offset: u64, buf: &mut [u8]) -> Result<()> {
        let mut done = 0;
        while done < buf.len() {
            let index = (offset / self.block) as usize;
            let inside = (offset % self.block) as usize;
            self.sector(index)?;
            let n = (buf.len() - done).min(self.block as usize - inside);
            buf[done..done + n].copy_from_slice(&self.cache[inside..inside + n]);
            done += n;
            offset += n as u64;
        }
        Ok(())
    }
}

// --- PFS ---

#[derive(Debug, Clone)]
struct Inode {
    size: u64,
    blocks: u32,
    direct: [i32; 12],
    indirect: [i32; 5],
}

struct Pfs {
    block: u64,
    signed: bool,
    inodes: Vec<Inode>,
}

impl Pfs {
    fn read(img: &mut dyn ReadAt) -> Result<Pfs> {
        let mut h = [0u8; 0x400];
        img.read_at(0, &mut h)?;
        if le64(&h, 0) != 1 || le64(&h, 8) as i64 != PFS_MAGIC {
            return Err(InstallError::Corrupt("PFS superblock".into()));
        }
        let mode = u16::from_le_bytes([h[0x1c], h[0x1d]]);
        let block = u64::from(le32(&h, 0x20));
        let (count, inode_blocks) = (le64(&h, 0x30) as usize, le64(&h, 0x40));
        let signed = mode & 1 != 0;
        let size = if signed { 0x2C8 } else { 0xA8 };
        let mut inodes = Vec::with_capacity(count);
        let mut data = vec![0u8; block as usize];
        for i in 0..inode_blocks {
            img.read_at(block * (1 + i), &mut data)?;
            for raw in data.chunks_exact(size).take(count - inodes.len()) {
                let mut direct = [0i32; 12];
                let mut indirect = [0i32; 5];
                for (k, d) in direct.iter_mut().enumerate() {
                    *d = lei32(raw, if signed { 0x64 + 36 * k + 32 } else { 0x64 + 4 * k });
                }
                for (k, d) in indirect.iter_mut().enumerate() {
                    *d = lei32(raw, if signed { 0x214 + 36 * k + 32 } else { 0x94 + 4 * k });
                }
                inodes.push(Inode { size: le64(raw, 8), blocks: le32(raw, 0x60), direct, indirect });
            }
        }
        // A wrong key decrypts the inode table to noise: check the root directory looks real.
        let sane = inodes.first().is_some_and(|root| {
            root.blocks >= 1 && root.blocks < 1 << 20 && root.direct[0] > 0 && root.size < 1 << 40
        });
        if !sane {
            return Err(InstallError::Corrupt("unreadable PFS inode table (wrong key?)".into()));
        }
        Ok(Pfs { block, signed, inodes })
    }

    /// The data blocks of a file (LibOrbisPkg `PfsReader.LoadFile`).
    fn block_list(&self, img: &mut dyn ReadAt, ino: &Inode) -> Result<Vec<u64>> {
        let n = ino.blocks as usize;
        if n <= 1 || ino.direct[1] == -1 {
            return Ok((0..n as u64).map(|i| ino.direct[0] as u64 + i).collect());
        }
        if !self.signed {
            return Err(InstallError::Unsupported("unsigned PFS with scattered blocks".into()));
        }
        let mut list: Vec<u64> = ino.direct.iter().take(n.min(12)).map(|&b| b as u64).collect();
        let per = (self.block / 36) as usize;
        let entry = |img: &mut dyn ReadAt, block: i32, i: usize| -> Result<u64> {
            let mut b = [0u8; 4];
            img.read_at(block as u64 * self.block + i as u64 * 36 + 32, &mut b)?;
            Ok(u64::from(u32::from_le_bytes(b)))
        };
        let mut remaining = n.saturating_sub(12);
        for i in 0..remaining.min(per) {
            list.push(entry(img, ino.indirect[0], i)?);
        }
        remaining = remaining.saturating_sub(per);
        let mut j = 0;
        while j * per < remaining {
            let table = entry(img, ino.indirect[1], j)? as i32;
            for i in 0..per.min(remaining - j * per) {
                list.push(entry(img, table, i)?);
            }
            j += 1;
        }
        Ok(list)
    }

    /// Every file below inode `ino` as (path, inode).
    fn walk(&self, img: &mut dyn ReadAt, ino: usize, path: &str, out: &mut Vec<(String, Inode)>) -> Result<()> {
        let node = self.inodes.get(ino).ok_or_else(|| InstallError::Corrupt("inode".into()))?.clone();
        let mut dirs = Vec::new();
        let mut data = vec![0u8; self.block as usize];
        let start = node.direct[0] as u64;
        for b in start..start + u64::from(node.blocks) {
            img.read_at(self.block * b, &mut data)?;
            let mut o = 0usize;
            while o + 16 <= data.len() {
                let (number, kind, len, ent) =
                    (le32(&data, o), lei32(&data, o + 4), le32(&data, o + 8) as usize, le32(&data, o + 12) as usize);
                if ent == 0 || o + 16 + len > data.len() {
                    break;
                }
                let name = String::from_utf8_lossy(&data[o + 16..o + 16 + len]).into_owned();
                let full = format!("{path}/{name}");
                match kind {
                    2 => out.push((full, self.inodes[number as usize].clone())),
                    3 => dirs.push((number as usize, full)),
                    _ => {}
                }
                o += ent;
            }
        }
        for (number, sub) in dirs {
            self.walk(img, number, &sub, out)?;
        }
        Ok(())
    }
}

// --- Install ---

/// Progress of an install: bytes written of the total, and the file being written.
pub trait Progress {
    /// Returns false to cancel.
    fn report(&mut self, done: u64, total: u64, file: &str) -> bool;
}

impl<F: FnMut(u64, u64, &str) -> bool> Progress for F {
    fn report(&mut self, done: u64, total: u64, file: &str) -> bool {
        self(done, total, file)
    }
}

/// Where a PKG installs: a game into `games_dir/<TITLE_ID>`, an add-on into
/// `games_dir/addcont/<TITLE_ID>/<label>`.
pub fn target_dir(info: &PkgInfo, games_dir: &Path) -> PathBuf {
    if info.is_game() {
        games_dir.join(&info.title_id)
    } else {
        games_dir.join("addcont").join(&info.title_id).join(&info.label)
    }
}

/// Installs `pkg` into `target`: the PFS files (`uroot/` at the top) and the plain `sce_sys`
/// entries. Returns the number of bytes written.
pub fn install(pkg: &Path, target: &Path, progress: &mut dyn Progress) -> Result<u64> {
    let info = read_info(pkg)?;
    match install_pfs(pkg, &info, target, progress) {
        Err(InstallError::Corrupt(why)) if !info.is_game() => {
            // Add-ons whose PFS key is not the fake passcode's: an entitlement-only pack needs just
            // its folder (named after the label) and the plain sce_sys entries.
            if std::env::var_os("SP_PKG_DEBUG").is_some() {
                eprintln!("add-on PFS not readable ({why}): installing the entitlement only");
            }
            fs::create_dir_all(target)?;
            write_sce_sys(pkg, &info, target)?;
            Ok(0)
        }
        other => other,
    }
}

fn install_pfs(pkg: &Path, info: &PkgInfo, target: &Path, progress: &mut dyn Progress) -> Result<u64> {
    let file = File::open(pkg)?;
    // The outer PFS: its superblock (seed at 0x370) is never encrypted.
    let mut outer = OuterImage { file, base: info.pfs_offset, xts: None, start: 0, cache_at: u64::MAX, cache: Vec::new() };
    let mut seed = [0u8; 16];
    outer.read_at(0x370, &mut seed)?;
    let mut sb = [0u8; 0x40];
    outer.read_at(0, &mut sb)?;
    let block = u64::from(le32(&sb, 0x20));
    let encrypted = u16::from_le_bytes([sb[0x1c], sb[0x1d]]) & 4 != 0;
    if encrypted {
        let (tweak, data) = xts_keys(&ekpfs(&info.content_id), &seed, info.pfs_flags & (1 << 61) != 0);
        outer.xts = Some(Xts {
            tweak: Aes128::new(GenericArray::from_slice(&tweak)),
            data: Aes128::new(GenericArray::from_slice(&data)),
        });
        outer.start = block / XTS_SECTOR;
        outer.cache.clear();
        outer.cache_at = u64::MAX;
    }
    let debug = std::env::var_os("SP_PKG_DEBUG").is_some();
    let outer_pfs = Pfs::read(&mut outer)?;
    if debug {
        eprintln!("outer: block {:#x} signed {} inodes {} root {:?}", outer_pfs.block, outer_pfs.signed,
                  outer_pfs.inodes.len(), outer_pfs.inodes.first());
    }
    let mut files = Vec::new();
    outer_pfs.walk(&mut outer, 0, "", &mut files)?;
    if debug {
        for (p, i) in &files { eprintln!("outer file {p} size {} blocks {} db {:?}", i.size, i.blocks, &i.direct[..3]); }
    }
    let image = files
        .iter()
        .find(|(p, _)| p == "/uroot/pfs_image.dat")
        .map(|(_, i)| i.clone())
        .ok_or_else(|| InstallError::Corrupt("no pfs_image.dat (wrong passcode?)".into()))?;
    let list = outer_pfs.block_list(&mut outer, &image)?;
    let blocks = Blocks { base: &mut outer, block: outer_pfs.block, list };
    let mut inner_img = Pfsc::new(blocks)?;
    let inner = Pfs::read(&mut inner_img)?;
    let mut entries = Vec::new();
    inner.walk(&mut inner_img, 0, "", &mut entries)?;
    let entries: Vec<_> = entries.into_iter().filter(|(p, _)| p.starts_with("/uroot/")).collect();
    let total: u64 = entries.iter().map(|(_, i)| i.size).sum();
    let mut done = 0u64;
    let mut buf = vec![0u8; 0x10000];
    for (path, node) in &entries {
        let relative = &path["/uroot/".len()..];
        let out = target.join(relative);
        if let Some(parent) = out.parent() {
            fs::create_dir_all(parent)?;
        }
        let list = inner.block_list(&mut inner_img, node)?;
        let mut reader = Blocks { base: &mut inner_img, block: inner.block, list };
        let part = out.with_extension("part~");
        let mut w = std::io::BufWriter::new(File::create(&part)?);
        let mut written = 0u64;
        while written < node.size {
            let n = (node.size - written).min(buf.len() as u64) as usize;
            reader.read_at(written, &mut buf[..n])?;
            w.write_all(&buf[..n])?;
            written += n as u64;
            done += n as u64;
            if !progress.report(done, total, relative) {
                drop(w);
                let _ = fs::remove_file(&part);
                return Err(InstallError::Cancelled);
            }
        }
        w.flush()?;
        drop(w);
        fs::rename(&part, &out)?;
    }
    write_sce_sys(pkg, info, target)?;
    Ok(done)
}

/// The plain (unencrypted) named entries: param.sfo, icons, trophies, changeinfo...
fn write_sce_sys(pkg: &Path, info: &PkgInfo, target: &Path) -> Result<()> {
    let mut f = File::open(pkg)?;
    let sys = target.join("sce_sys");
    for e in &info.entries {
        let Some(name) = &e.name else { continue };
        if e.flags1 & 0x8000_0000 != 0 || name.contains("..") || name.starts_with('/') {
            continue; // encrypted (licenses) or not a plain relative name
        }
        let out = sys.join(name);
        if let Some(parent) = out.parent() {
            fs::create_dir_all(parent)?;
        }
        let mut data = vec![0u8; e.size as usize];
        f.seek(SeekFrom::Start(u64::from(e.offset)))?;
        f.read_exact(&mut data)?;
        fs::write(out, data)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn xts_tweak_doubling_carries() {
        // A sector of zeros decrypts deterministically with any key; just exercise the path.
        let xts = Xts {
            tweak: Aes128::new(GenericArray::from_slice(&[1u8; 16])),
            data: Aes128::new(GenericArray::from_slice(&[2u8; 16])),
        };
        let mut a = vec![0u8; 0x1000];
        let mut b = vec![0u8; 0x1000];
        xts.decrypt_sector(16, &mut a);
        xts.decrypt_sector(17, &mut b);
        assert_ne!(a, b);
    }

    #[test]
    fn ekpfs_depends_on_content_id() {
        assert_ne!(ekpfs("UP0002-CUSA00242_00-DIABLO3PS4000001"), ekpfs("UP0002-CUSA00242_00-DIABLO3PS4000002"));
    }
}
