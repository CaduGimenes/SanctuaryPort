//! Read-only access to Diablo III's console archives (`CPKs/*.cpk`, Blizzard's own format,
//! version 7) and the string lists inside them (`StringList\*.stl`).
//!
//! The same reader as `scripts/d3_cpk.py` (after github.com/RedMadKnight/D3CPKUnpack): a 72-byte
//! little-endian header, then bit-packed tables (most significant bit first): per file a 64-bit
//! FNV-1a hash of the lower-case path, its size, copy count and first location; the
//! decompressed-stream offset of each location; the decompressed offset of each compressed sector;
//! the compressed sector of each decompressed one. Then one u32 name offset per file and the names.
//! Data: compressed sectors from the first data byte, each a list of zlib chunks (u16 size low,
//! u16 size high, u16 stored size; 0 ends the sector). Nothing is written.

use std::collections::HashMap;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

const MAGIC: u32 = 0xA1B2_C3D4;
const HEADER_SIZE: usize = 72;
/// Header and tables bigger than this are not a real archive (the game's are a few MiB).
const MAX_TABLES: u64 = 256 << 20;
/// No game file read this way is bigger (the string lists are a few hundred KiB).
const MAX_FILE: u64 = 64 << 20;

fn bad(what: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, what.to_string())
}

/// The 64-bit FNV-1a hash of the lower-case name (in Latin-1), as the tables store it.
pub fn fnv1a64(name: &str) -> u64 {
    name.chars()
        .flat_map(char::to_lowercase)
        .map(|c| u32::from(c) as u8)
        .fold(0xCBF2_9CE4_8422_2325, |hash, byte| {
            (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01B3)
        })
}

/// Fields of up to 64 bits, most significant bit first.
struct Bits<'a> {
    data: &'a [u8],
    bit: usize,
}

impl Bits<'_> {
    fn read(&mut self, width: u32) -> io::Result<u64> {
        let mut value = 0u64;
        for _ in 0..width {
            let byte = *self.data.get(self.bit >> 3).ok_or_else(|| bad("tables cut short"))?;
            value = value << 1 | u64::from(byte >> (7 - (self.bit & 7)) & 1);
            self.bit += 1;
        }
        Ok(value)
    }

    fn table(&mut self, count: usize, width: u32) -> io::Result<Vec<u64>> {
        (0..count).map(|_| self.read(width)).collect()
    }

    /// The byte after the last field read.
    fn end(&self) -> usize {
        self.bit.div_ceil(8)
    }
}

/// One open archive: its tables and file names.
pub struct Archive {
    file: File,
    first: u64,
    sector_size: u64,
    /// Per file: hash, size, first location.
    entries: Vec<(u64, u64, usize)>,
    locations: Vec<u64>,
    cs2do: Vec<u64>,
    ds2cs: Vec<u64>,
    names: HashMap<String, usize>,
}

impl Archive {
    pub fn open(path: &Path) -> io::Result<Archive> {
        let mut file = File::open(path)?;
        let length = file.metadata()?.len();
        let mut head = [0u8; HEADER_SIZE];
        file.read_exact(&mut head)?;
        let u32_at = |at: usize| u32::from_le_bytes(head[at..at + 4].try_into().unwrap());
        let field = |index: usize| u32_at(16 + 4 * index);
        if u32_at(0) != MAGIC || u32_at(4) != 7 {
            return Err(bad("not a version 7 Diablo III archive"));
        }
        let decomp_size = u64::from_le_bytes(head[8..16].try_into().unwrap());
        let (layout, files, location_count, header_sector) = (field(0), field(1), field(2), field(3));
        let (size_bits, count_bits, index_bits, location_bits) = (field(4), field(5), field(6), field(7));
        let (cs2do_bits, ds2cs_bits, read_sector, sector_size) = (field(8), field(9), field(11), field(12));
        if [size_bits, count_bits, index_bits, location_bits, cs2do_bits, ds2cs_bits]
            .iter()
            .any(|&bits| bits > 64)
            || sector_size == 0
        {
            return Err(bad("odd archive header"));
        }
        let first = if layout == 10 {
            u64::from(header_sector)
        } else {
            u64::from(header_sector) * u64::from(read_sector)
        };
        if first > length || first > MAX_TABLES || first < HEADER_SIZE as u64 {
            return Err(bad("odd archive header"));
        }
        let sector_size = u64::from(sector_size);
        let mut data = vec![0u8; first as usize];
        file.seek(SeekFrom::Start(0))?;
        file.read_exact(&mut data)?;

        let files = files as usize;
        let mut bits = Bits { data: &data, bit: HEADER_SIZE * 8 };
        let mut entries = Vec::with_capacity(files.min(1 << 20));
        for _ in 0..files {
            let hash = bits.read(64)?;
            let size = bits.read(size_bits)?;
            bits.read(count_bits)?;
            let index = bits.read(index_bits)? as usize;
            entries.push((hash, size, index));
        }
        let mut bits = Bits { data: &data, bit: bits.end() * 8 };
        let locations = bits.table(location_count as usize, location_bits)?;
        let sectors = ((length - first) / sector_size) as usize;
        let mut bits = Bits { data: &data, bit: bits.end() * 8 };
        let cs2do = bits.table(sectors, cs2do_bits)?;
        let mut bits = Bits { data: &data, bit: bits.end() * 8 };
        let ds2cs = bits.table(decomp_size.div_ceil(sector_size) as usize, ds2cs_bits)?;
        let mut pos = bits.end();
        let mut offsets = Vec::with_capacity(files.min(1 << 20));
        for _ in 0..files {
            let raw = data.get(pos..pos + 4).ok_or_else(|| bad("names cut short"))?;
            offsets.push(u32::from_le_bytes(raw.try_into().unwrap()) as usize);
            pos += 4;
        }
        let mut names = HashMap::with_capacity(files);
        for (index, offset) in offsets.into_iter().enumerate() {
            let start = pos.checked_add(offset).ok_or_else(|| bad("names cut short"))?;
            let tail = data.get(start..).ok_or_else(|| bad("names cut short"))?;
            let end = tail.iter().position(|&b| b == 0).ok_or_else(|| bad("names cut short"))?;
            // Latin-1, as the Python reader decodes them.
            names.insert(tail[..end].iter().map(|&b| char::from(b)).collect(), index);
        }
        Ok(Archive {
            file,
            first,
            sector_size,
            entries,
            locations,
            cs2do,
            ds2cs,
            names,
        })
    }

    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.names.keys().map(String::as_str)
    }

    fn sector(&mut self, index: usize) -> io::Result<Vec<u8>> {
        let mut blob = vec![0u8; self.sector_size as usize];
        self.file
            .seek(SeekFrom::Start(self.first + index as u64 * self.sector_size))?;
        let got = read_up_to(&mut self.file, &mut blob)?;
        blob.truncate(got);
        let mut out = Vec::new();
        let mut pos = 0;
        while pos + 6 <= blob.len() {
            let half = |at: usize| u16::from_le_bytes([blob[at], blob[at + 1]]);
            let (low, high, stored) = (half(pos), half(pos + 2), half(pos + 4));
            if stored == 0 {
                break;
            }
            if stored & 0x8000 != 0 {
                return Err(bad("encrypted chunk"));
            }
            let size = usize::from(low) | usize::from(high) << 16;
            let packed = blob
                .get(pos + 6..pos + 6 + usize::from(stored))
                .ok_or_else(|| bad("chunk cut short"))?;
            let mut chunk = Vec::new();
            // Like Python's decompressobj().decompress: what decodes before an error or the end.
            let _ = flate2::read::ZlibDecoder::new(packed).read_to_end(&mut chunk);
            if size > 0 {
                chunk.resize(size, 0);
            }
            out.extend_from_slice(&chunk);
            pos += 6 + usize::from(stored);
        }
        Ok(out)
    }

    fn read_at(&mut self, offset: u64, size: u64) -> io::Result<Vec<u8>> {
        let lookup = (offset / self.sector_size) as usize;
        let mut sector = *self.ds2cs.get(lookup).ok_or_else(|| bad("offset past the end"))? as usize;
        if sector >= self.cs2do.len() {
            return Err(bad("offset past the end"));
        }
        while sector > 0 && self.cs2do[sector] > offset {
            sector -= 1;
        }
        let start = self.cs2do[sector];
        if start > offset {
            return Err(bad("offset before the data"));
        }
        let mut buf: Vec<u8> = Vec::new();
        while start + (buf.len() as u64) < offset + size {
            if sector >= self.cs2do.len() {
                return Err(bad("file cut short"));
            }
            let data = self.sector(sector)?;
            buf.extend_from_slice(&data);
            sector += 1;
            // The table decides where the next sector starts.
            if sector < self.cs2do.len() {
                let have = start + buf.len() as u64;
                let next = self.cs2do[sector];
                if next > have {
                    buf.resize(buf.len() + (next - have) as usize, 0);
                } else {
                    buf.truncate(buf.len() - (have - next).min(buf.len() as u64) as usize);
                }
            } else if data.is_empty() {
                return Err(bad("file cut short"));
            }
        }
        let from = (offset - start) as usize;
        Ok(buf[from..from + size as usize].to_vec())
    }

    /// The bytes of file `name` (`NotFound` when the archive has no such name).
    pub fn read(&mut self, name: &str) -> io::Result<Vec<u8>> {
        let index = *self
            .names
            .get(name)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, name.to_string()))?;
        let (hash, size, location) = *self.entries.get(index).ok_or_else(|| bad("odd file index"))?;
        if hash != fnv1a64(name) {
            return Err(bad("hash mismatch"));
        }
        if size > MAX_FILE {
            return Err(bad("file too big"));
        }
        let offset = *self.locations.get(location).ok_or_else(|| bad("odd location"))?;
        self.read_at(offset, size)
    }
}

fn read_up_to(file: &mut File, buf: &mut [u8]) -> io::Result<usize> {
    let mut got = 0;
    while got < buf.len() {
        match file.read(&mut buf[got..])? {
            0 => break,
            n => got += n,
        }
    }
    Ok(got)
}

/// {label hash: text} of a string list (`.stl`): after a 16-byte file header, the table's offset and
/// size at 0x28; 24-byte entries (label offset and size, text offset and size, label hash);
/// offsets count from 0x10.
pub fn string_list(data: &[u8]) -> HashMap<u32, String> {
    let u32_at = |at: usize| {
        data.get(at..at.checked_add(4)?)
            .map(|raw| u32::from_le_bytes(raw.try_into().unwrap()) as usize)
    };
    let mut texts = HashMap::new();
    let (Some(table), Some(size)) = (u32_at(0x28), u32_at(0x2c)) else {
        return texts;
    };
    for index in 0..size / 0x18 {
        let entry = 0x10 + table + index * 0x18;
        let (Some(offset), Some(length), Some(hash)) =
            (u32_at(entry + 8), u32_at(entry + 12), u32_at(entry + 16))
        else {
            break;
        };
        let Some(raw) = data.get(0x10 + offset..0x10 + offset + length) else {
            continue;
        };
        let raw = raw.split(|&b| b == 0).next().unwrap_or_default();
        texts.insert(hash as u32, String::from_utf8_lossy(raw).into_owned());
    }
    texts
}

/// Synthesised archives and string lists for the tests (never real game data).
#[doc(hidden)]
pub mod fixtures {
    use super::*;
    use std::io::Write;

    /// Most significant bit first, as the reader expects.
    #[derive(Default)]
    struct BitWriter {
        out: Vec<u8>,
        bit: usize,
    }

    impl BitWriter {
        fn put(&mut self, value: u64, width: u32) {
            for i in (0..width).rev() {
                if self.bit % 8 == 0 {
                    self.out.push(0);
                }
                if value >> i & 1 == 1 {
                    *self.out.last_mut().unwrap() |= 0x80 >> (self.bit % 8);
                }
                self.bit += 1;
            }
        }

        fn align(&mut self) {
            self.bit = self.out.len() * 8;
        }
    }

    /// A synthesised version 7 archive holding `files` (one decompressed sector per compressed one).
    pub fn cpk(files: &[(&str, Vec<u8>)]) -> Vec<u8> {
        const SECTOR: usize = 0x400;
        let mut stream = Vec::new();
        let mut placed = Vec::new();
        for (name, data) in files {
            placed.push((name.to_string(), stream.len() as u64, data.len() as u64));
            stream.extend_from_slice(data);
        }
        let mut sectors = Vec::new();
        for piece in stream.chunks(SECTOR) {
            let mut zlib = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::best());
            zlib.write_all(piece).unwrap();
            let packed = zlib.finish().unwrap();
            assert!(packed.len() + 12 <= SECTOR && packed.len() < 0x8000);
            let mut sector = Vec::new();
            sector.extend((piece.len() as u16).to_le_bytes());
            sector.extend(((piece.len() >> 16) as u16).to_le_bytes());
            sector.extend((packed.len() as u16).to_le_bytes());
            sector.extend(packed);
            sector.resize(SECTOR, 0);
            sectors.push(sector);
        }
        let (size_bits, count_bits, index_bits, location_bits, cs2do_bits, ds2cs_bits) = (32, 4, 16, 32, 32, 16);
        let mut bits = BitWriter::default();
        for (index, (name, _, size)) in placed.iter().enumerate() {
            bits.put(fnv1a64(name), 64);
            bits.put(*size, size_bits);
            bits.put(1, count_bits);
            bits.put(index as u64, index_bits);
        }
        bits.align();
        for (_, offset, _) in &placed {
            bits.put(*offset, location_bits);
        }
        bits.align();
        for index in 0..sectors.len() {
            bits.put((index * SECTOR) as u64, cs2do_bits);
        }
        bits.align();
        for index in 0..sectors.len() {
            bits.put(index as u64, ds2cs_bits);
        }
        let mut tables = bits.out;
        let mut names = Vec::new();
        for (name, _, _) in &placed {
            tables.extend((names.len() as u32).to_le_bytes());
            names.extend(name.as_bytes());
            names.push(0);
        }
        tables.extend(names);
        let first = (HEADER_SIZE + tables.len()).div_ceil(16) * 16;
        let mut out = Vec::new();
        out.extend(MAGIC.to_le_bytes());
        out.extend(7u32.to_le_bytes());
        out.extend((stream.len() as u64).to_le_bytes());
        let fields = [
            10, // layout: `first` counts bytes
            placed.len() as u32,
            placed.len() as u32,
            first as u32,
            size_bits,
            count_bits,
            index_bits,
            location_bits,
            cs2do_bits,
            ds2cs_bits,
            0,
            1,
            SECTOR as u32,
            0,
        ];
        for field in fields {
            out.extend(field.to_le_bytes());
        }
        out.extend(tables);
        out.resize(first, 0);
        for sector in sectors {
            out.extend(sector);
        }
        out
    }

    /// A synthesised string list with these (label hash, text) entries.
    pub fn stl(entries: &[(u32, &str)]) -> Vec<u8> {
        let table = 0x28usize;
        let texts_at = table + entries.len() * 0x18;
        let mut head = vec![0u8; 0x10 + table];
        head[..4].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
        head[0x28..0x2c].copy_from_slice(&(table as u32).to_le_bytes());
        head[0x2c..0x30].copy_from_slice(&((entries.len() * 0x18) as u32).to_le_bytes());
        let mut rows = Vec::new();
        let mut texts = Vec::new();
        for (hash, text) in entries {
            let offset = texts_at + texts.len();
            let mut bytes = text.as_bytes().to_vec();
            bytes.push(0);
            rows.extend([0u32, 0, offset as u32, bytes.len() as u32, *hash, 0].iter().flat_map(|v| v.to_le_bytes()));
            texts.extend(bytes);
            while texts.len() % 8 != 0 {
                texts.push(0);
            }
        }
        head.extend(rows);
        head.extend(texts);
        head
    }
}

#[cfg(test)]
mod tests {
    use super::fixtures::{cpk, stl};
    use super::*;

    #[test]
    fn files_come_back_from_a_synthesised_archive() {
        let dir = tempfile::tempdir().unwrap();
        let big: Vec<u8> = (0..5000u32).map(|i| (i * 7 % 251) as u8).collect();
        let path = dir.path().join("test.cpk");
        std::fs::write(
            &path,
            cpk(&[
                ("StringList\\A.stl", b"first file".to_vec()),
                ("Data\\Big.bin", big.clone()),
                ("StringList\\B.stl", b"after the big one".to_vec()),
            ]),
        )
        .unwrap();
        let mut archive = Archive::open(&path).unwrap();
        assert_eq!(archive.names().count(), 3);
        assert_eq!(archive.read("StringList\\A.stl").unwrap(), b"first file");
        assert_eq!(archive.read("Data\\Big.bin").unwrap(), big);
        assert_eq!(archive.read("StringList\\B.stl").unwrap(), b"after the big one");
        let missing = archive.read("StringList\\C.stl").unwrap_err();
        assert_eq!(missing.kind(), io::ErrorKind::NotFound);
    }

    #[test]
    fn bad_archives_are_refused() {
        let dir = tempfile::tempdir().unwrap();
        let good = cpk(&[("a", b"abc".to_vec())]);
        let mut wrong = good.clone();
        wrong[0] ^= 1;
        let path = dir.path().join("bad.cpk");
        std::fs::write(&path, &wrong).unwrap();
        assert!(Archive::open(&path).is_err());
        std::fs::write(&path, &good[..40]).unwrap();
        assert!(Archive::open(&path).is_err());
        std::fs::write(&path, &good[..100]).unwrap();
        assert!(Archive::open(&path).is_err());
        assert!(Archive::open(&dir.path().join("none.cpk")).is_err());
    }

    #[test]
    fn string_lists_map_label_hashes_to_texts() {
        let texts = string_list(&stl(&[(0x1234_5678, "Глубокие карманы"), (7, "")]));
        assert_eq!(texts[&0x1234_5678], "Глубокие карманы");
        assert_eq!(texts[&7], "");
        assert!(string_list(b"short").is_empty());
    }

    #[test]
    fn the_hash_is_fnv1a_of_the_lower_case_name() {
        assert_eq!(fnv1a64(""), 0xCBF2_9CE4_8422_2325);
        assert_eq!(fnv1a64("StringList\\A.stl"), fnv1a64("stringlist\\a.STL"));
    }
}
