use flate2::bufread::DeflateDecoder;
use std::collections::HashSet;
use std::env;
use std::fs;
use std::io::{Cursor, Read};
use std::path::Path;

type Result<T> = std::result::Result<T, String>;

const MAX_APK_SIZE: usize = 512 * 1024 * 1024;
// Modern resource-heavy applications such as Chromium exceed 4,096 archive
// members without using ZIP64. Keep a bounded ceiling above that real-world
// shape; the classic EOCD entry count still provides a hard 65,535 limit.
const MAX_ENTRIES: usize = 8192;
const MAX_MANIFEST_SIZE: usize = 4 * 1024 * 1024;
const MAX_DEX_SIZE: usize = 128 * 1024 * 1024;
// Chromium's monolithic arm64 libchrome.so is roughly 240 MiB. This remains
// bounded below the APK-wide 512 MiB cap and is only allocated while examining
// a selected native entry.
const MAX_NATIVE_LIBRARY_SIZE: usize = 256 * 1024 * 1024;
const EOCD: u32 = 0x0605_4b50;
const CENTRAL: u32 = 0x0201_4b50;
const LOCAL: u32 = 0x0403_4b50;
const RES_STRING_POOL: u16 = 0x0001;
const RES_XML: u16 = 0x0003;
const RES_XML_START_ELEMENT: u16 = 0x0102;
const RES_XML_END_ELEMENT: u16 = 0x0103;
const UTF8_FLAG: u32 = 0x0000_0100;
const TYPE_REFERENCE: u8 = 0x01;
const TYPE_STRING: u8 = 0x03;
const TYPE_INT_DEC: u8 = 0x10;
const TYPE_INT_HEX: u8 = 0x11;
const TYPE_INT_BOOLEAN: u8 = 0x12;

#[derive(Clone)]
struct ZipEntry {
    name: Vec<u8>,
    flags: u16,
    method: u16,
    crc32: u32,
    compressed_size: usize,
    uncompressed_size: usize,
    local_offset: usize,
}

struct StringPool {
    strings: Vec<String>,
}

#[derive(Clone)]
struct ActivityCandidate {
    depth: usize,
    component_name: String,
    name: String,
    theme: Option<u32>,
    label: Option<String>,
    label_res: Option<u32>,
    main: bool,
    launcher: bool,
    alias: bool,
}

#[derive(Clone)]
struct ServiceCandidate {
    name: String,
    process: Option<String>,
}

#[derive(Clone)]
enum ManifestMetadataValue {
    Resource(u32),
    Integer(u32),
    Boolean(bool),
    String(String),
}

#[derive(Clone)]
struct ManifestMetadata {
    name: String,
    value: ManifestMetadataValue,
}

struct ManifestInfo {
    package: String,
    application: String,
    activity: String,
    launch_component: String,
    activity_themes: Vec<(String, u32)>,
    activity_aliases: Vec<(String, String)>,
    services: Vec<(String, String)>,
    application_metadata: Vec<ManifestMetadata>,
    version_code: u32,
    version_name: String,
    theme: u32,
    target_sdk: u32,
    label: String,
    label_res: u32,
    icon: Option<String>,
}

fn find_metadata_value(
    input: &[u8],
    pool: &StringPool,
    attrs: usize,
    count: usize,
    attr_size: usize,
) -> Result<Option<ManifestMetadataValue>> {
    if attr_size < 20 {
        return Err("manifest attribute record is too small".to_owned());
    }
    for index in 0..count {
        let attr = checked_add(attrs, index * attr_size, "manifest attribute")?;
        if pool_string(pool, u32le(input, attr + 4, "attribute name")?)? != "value" {
            continue;
        }
        let raw = u32le(input, attr + 8, "attribute raw value")?;
        let value_size = u16le(input, attr + 12, "attribute value size")?;
        let value_type = *bytes(input, attr + 15, 1, "attribute type")?
            .first()
            .ok_or_else(|| "attribute type is absent".to_owned())?;
        let data = u32le(input, attr + 16, "attribute data")?;
        if value_size != 8 {
            return Ok(None);
        }
        return Ok(match value_type {
            TYPE_REFERENCE => Some(ManifestMetadataValue::Resource(data)),
            TYPE_INT_DEC | TYPE_INT_HEX => Some(ManifestMetadataValue::Integer(data)),
            TYPE_INT_BOOLEAN => Some(ManifestMetadataValue::Boolean(data != 0)),
            TYPE_STRING => Some(ManifestMetadataValue::String(
                pool_string(pool, data)?.to_owned(),
            )),
            _ if raw != u32::MAX => Some(ManifestMetadataValue::String(
                pool_string(pool, raw)?.to_owned(),
            )),
            _ => None,
        });
    }
    Ok(None)
}

fn checked_add(a: usize, b: usize, label: &str) -> Result<usize> {
    a.checked_add(b)
        .ok_or_else(|| format!("{label} offset overflow"))
}

fn bytes<'a>(input: &'a [u8], offset: usize, size: usize, label: &str) -> Result<&'a [u8]> {
    let end = checked_add(offset, size, label)?;
    input
        .get(offset..end)
        .ok_or_else(|| format!("{label} exceeds input bounds"))
}

fn u16le(input: &[u8], offset: usize, label: &str) -> Result<u16> {
    let value = bytes(input, offset, 2, label)?;
    Ok(u16::from_le_bytes([value[0], value[1]]))
}

fn u32le(input: &[u8], offset: usize, label: &str) -> Result<u32> {
    let value = bytes(input, offset, 4, label)?;
    Ok(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

fn u64le(input: &[u8], offset: usize, label: &str) -> Result<u64> {
    let value = bytes(input, offset, 8, label)?;
    Ok(u64::from_le_bytes(value.try_into().unwrap()))
}

fn safe_name(name: &[u8]) -> Result<()> {
    let directory = name.ends_with(b"/");
    let body = if directory {
        &name[..name.len().saturating_sub(1)]
    } else {
        name
    };
    if name.is_empty()
        || name[0] == b'/'
        || name.contains(&0)
        || name.contains(&b'\\')
        || body.is_empty()
        || body
            .split(|byte| *byte == b'/')
            .any(|part| part.is_empty() || part == b"." || part == b"..")
    {
        return Err("APK contains an unsafe ZIP entry name".to_owned());
    }
    Ok(())
}

fn find_eocd(input: &[u8]) -> Result<usize> {
    if input.len() < 22 {
        return Err("APK is shorter than a ZIP EOCD".to_owned());
    }
    let floor = input.len().saturating_sub(22 + usize::from(u16::MAX));
    for offset in (floor..=input.len() - 22).rev() {
        if u32le(input, offset, "EOCD signature")? != EOCD {
            continue;
        }
        let comment = usize::from(u16le(input, offset + 20, "EOCD comment")?);
        if checked_add(offset + 22, comment, "EOCD end")? == input.len() {
            return Ok(offset);
        }
    }
    Err("terminal ZIP EOCD was not found".to_owned())
}

fn parse_zip(input: &[u8]) -> Result<Vec<ZipEntry>> {
    let eocd = find_eocd(input)?;
    let disk = u16le(input, eocd + 4, "EOCD disk")?;
    let central_disk = u16le(input, eocd + 6, "EOCD central disk")?;
    let disk_count = u16le(input, eocd + 8, "EOCD disk count")?;
    let count = u16le(input, eocd + 10, "EOCD count")?;
    if disk != 0 || central_disk != 0 || disk_count != count {
        return Err("multi-disk APKs are unsupported".to_owned());
    }
    if usize::from(count) > MAX_ENTRIES {
        return Err(format!("APK exceeds the {MAX_ENTRIES}-entry cap"));
    }
    let central_size = u32le(input, eocd + 12, "central size")? as usize;
    let central_offset = u32le(input, eocd + 16, "central offset")? as usize;
    if central_size == u32::MAX as usize || central_offset == u32::MAX as usize {
        return Err("ZIP64 APKs are unsupported".to_owned());
    }
    let central_end = checked_add(central_offset, central_size, "central directory")?;
    if central_end != eocd {
        return Err("central directory does not end at EOCD".to_owned());
    }
    let mut offset = central_offset;
    let mut names = HashSet::new();
    let mut entries = Vec::with_capacity(usize::from(count));
    for _ in 0..count {
        if u32le(input, offset, "central signature")? != CENTRAL {
            return Err("central directory signature mismatch".to_owned());
        }
        let flags = u16le(input, offset + 8, "central flags")?;
        let method = u16le(input, offset + 10, "central method")?;
        let crc32 = u32le(input, offset + 16, "central CRC")?;
        let compressed = u32le(input, offset + 20, "central compressed")?;
        let uncompressed = u32le(input, offset + 24, "central uncompressed")?;
        let name_len = usize::from(u16le(input, offset + 28, "central name length")?);
        let extra_len = usize::from(u16le(input, offset + 30, "central extra length")?);
        let comment_len = usize::from(u16le(input, offset + 32, "central comment length")?);
        let start_disk = u16le(input, offset + 34, "entry start disk")?;
        let local = u32le(input, offset + 42, "local offset")?;
        if compressed == u32::MAX || uncompressed == u32::MAX || local == u32::MAX {
            return Err("ZIP64 entries are unsupported".to_owned());
        }
        if start_disk != 0 || flags & 0x2041 != 0 {
            return Err("encrypted, masked, or multi-disk entries are unsupported".to_owned());
        }
        let name_start = checked_add(offset, 46, "central fixed header")?;
        let name = bytes(input, name_start, name_len, "central name")?.to_vec();
        safe_name(&name)?;
        if !names.insert(name.clone()) {
            return Err("duplicate APK entry name".to_owned());
        }
        offset = checked_add(
            checked_add(
                checked_add(name_start, name_len, "central name")?,
                extra_len,
                "central extra",
            )?,
            comment_len,
            "central comment",
        )?;
        if offset > central_end {
            return Err("central entry exceeds directory bounds".to_owned());
        }
        entries.push(ZipEntry {
            name,
            flags,
            method,
            crc32,
            compressed_size: compressed as usize,
            uncompressed_size: uncompressed as usize,
            local_offset: local as usize,
        });
    }
    if offset != central_end {
        return Err("central entry count/size mismatch".to_owned());
    }
    Ok(entries)
}

fn crc32(input: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for byte in input {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & 0_u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

fn entry_bytes(input: &[u8], entry: &ZipEntry, cap: usize) -> Result<Vec<u8>> {
    if entry.uncompressed_size == 0 || entry.uncompressed_size > cap {
        return Err(format!("APK entry is outside the 1..={cap} byte cap"));
    }
    let offset = entry.local_offset;
    if u32le(input, offset, "local signature")? != LOCAL {
        return Err("local header signature mismatch".to_owned());
    }
    let flags = u16le(input, offset + 6, "local flags")?;
    let method = u16le(input, offset + 8, "local method")?;
    let name_len = usize::from(u16le(input, offset + 26, "local name length")?);
    let extra_len = usize::from(u16le(input, offset + 28, "local extra length")?);
    let name_start = checked_add(offset, 30, "local fixed header")?;
    if bytes(input, name_start, name_len, "local name")? != entry.name {
        return Err("central/local entry name mismatch".to_owned());
    }
    if flags != entry.flags || method != entry.method {
        return Err("central/local flags or method mismatch".to_owned());
    }
    let data_start = checked_add(
        checked_add(name_start, name_len, "local name")?,
        extra_len,
        "local extra",
    )?;
    let compressed = bytes(input, data_start, entry.compressed_size, "entry data")?;
    let mut output = Vec::with_capacity(entry.uncompressed_size);
    match entry.method {
        0 => output.extend_from_slice(compressed),
        8 => {
            let mut decoder = DeflateDecoder::new(Cursor::new(compressed));
            decoder
                .by_ref()
                .take((cap + 1) as u64)
                .read_to_end(&mut output)
                .map_err(|error| format!("deflate decode failed: {error}"))?;
            if decoder.total_in() != compressed.len() as u64 {
                return Err("deflate stream did not consume its exact range".to_owned());
            }
        }
        _ => return Err("manifest/classes.dex compression method is unsupported".to_owned()),
    }
    if output.len() != entry.uncompressed_size || crc32(&output) != entry.crc32 {
        return Err("APK entry size or CRC mismatch".to_owned());
    }
    Ok(output)
}

fn length8(input: &[u8], cursor: &mut usize) -> Result<usize> {
    let first = *bytes(input, *cursor, 1, "UTF-8 length")?
        .first()
        .ok_or_else(|| "UTF-8 length is absent".to_owned())?;
    *cursor += 1;
    if first & 0x80 == 0 {
        Ok(usize::from(first))
    } else {
        let second = *bytes(input, *cursor, 1, "UTF-8 length tail")?
            .first()
            .ok_or_else(|| "UTF-8 length tail is absent".to_owned())?;
        *cursor += 1;
        Ok((usize::from(first & 0x7f) << 8) | usize::from(second))
    }
}

fn length16(input: &[u8], cursor: &mut usize) -> Result<usize> {
    let first = u16le(input, *cursor, "UTF-16 length")?;
    *cursor += 2;
    if first & 0x8000 == 0 {
        Ok(usize::from(first))
    } else {
        let second = u16le(input, *cursor, "UTF-16 length tail")?;
        *cursor += 2;
        Ok((usize::from(first & 0x7fff) << 16) | usize::from(second))
    }
}

fn parse_string_pool(input: &[u8], offset: usize, size: usize) -> Result<StringPool> {
    let header_size = usize::from(u16le(input, offset + 2, "string header size")?);
    if header_size < 28 || size < header_size {
        return Err("invalid string-pool header".to_owned());
    }
    let count = u32le(input, offset + 8, "string count")? as usize;
    if count > 65_536 {
        return Err("manifest string pool is too large".to_owned());
    }
    let flags = u32le(input, offset + 16, "string flags")?;
    let strings_start = u32le(input, offset + 20, "strings start")? as usize;
    let offsets_start = checked_add(offset, header_size, "string offsets")?;
    let strings_base = checked_add(offset, strings_start, "string data")?;
    let chunk_end = checked_add(offset, size, "string chunk")?;
    let mut strings = Vec::with_capacity(count);
    for index in 0..count {
        let relative = u32le(input, offsets_start + index * 4, "string offset")? as usize;
        let mut cursor = checked_add(strings_base, relative, "string")?;
        if cursor >= chunk_end {
            return Err("manifest string offset exceeds pool".to_owned());
        }
        if flags & UTF8_FLAG != 0 {
            let _utf16_length = length8(input, &mut cursor)?;
            let byte_length = length8(input, &mut cursor)?;
            let raw = bytes(input, cursor, byte_length, "UTF-8 string")?;
            let value =
                std::str::from_utf8(raw).map_err(|_| "manifest string is not UTF-8".to_owned())?;
            strings.push(value.to_owned());
        } else {
            let units = length16(input, &mut cursor)?;
            let mut value = Vec::with_capacity(units);
            for unit in 0..units {
                value.push(u16le(input, cursor + unit * 2, "UTF-16 string")?);
            }
            strings.push(
                String::from_utf16(&value)
                    .map_err(|_| "manifest string is invalid UTF-16".to_owned())?,
            );
        }
    }
    Ok(StringPool { strings })
}

fn pool_string(pool: &StringPool, index: u32) -> Result<&str> {
    if index == u32::MAX {
        return Err("manifest references a missing string".to_owned());
    }
    pool.strings
        .get(index as usize)
        .map(String::as_str)
        .ok_or_else(|| "manifest string index is out of range".to_owned())
}

fn attribute_value(input: &[u8], pool: &StringPool, attr: usize) -> Result<Option<String>> {
    let raw = u32le(input, attr + 8, "attribute raw value")?;
    let value_size = u16le(input, attr + 12, "attribute value size")?;
    let value_type = *bytes(input, attr + 15, 1, "attribute type")?
        .first()
        .ok_or_else(|| "attribute type is absent".to_owned())?;
    let data = u32le(input, attr + 16, "attribute data")?;
    if raw != u32::MAX {
        return Ok(Some(pool_string(pool, raw)?.to_owned()));
    }
    if value_size == 8 && value_type == TYPE_STRING {
        return Ok(Some(pool_string(pool, data)?.to_owned()));
    }
    Ok(None)
}

fn find_attribute(
    input: &[u8],
    pool: &StringPool,
    attrs: usize,
    count: usize,
    attr_size: usize,
    wanted: &str,
) -> Result<Option<String>> {
    if attr_size < 20 {
        return Err("manifest attribute record is too small".to_owned());
    }
    for index in 0..count {
        let attr = checked_add(attrs, index * attr_size, "manifest attribute")?;
        let name = pool_string(pool, u32le(input, attr + 4, "attribute name")?)?;
        if name == wanted {
            return attribute_value(input, pool, attr);
        }
    }
    Ok(None)
}

fn find_resource_attribute(
    input: &[u8],
    pool: &StringPool,
    attrs: usize,
    count: usize,
    attr_size: usize,
    wanted: &str,
) -> Result<Option<u32>> {
    if attr_size < 20 {
        return Err("manifest attribute record is too small".to_owned());
    }
    for index in 0..count {
        let attr = checked_add(attrs, index * attr_size, "manifest attribute")?;
        let name = pool_string(pool, u32le(input, attr + 4, "attribute name")?)?;
        if name != wanted {
            continue;
        }
        let value_size = u16le(input, attr + 12, "attribute value size")?;
        let value_type = *bytes(input, attr + 15, 1, "attribute type")?
            .first()
            .ok_or_else(|| "attribute type is absent".to_owned())?;
        return if value_size == 8 && value_type == TYPE_REFERENCE {
            Ok(Some(u32le(input, attr + 16, "attribute resource")?))
        } else {
            Ok(None)
        };
    }
    Ok(None)
}

fn find_integer_attribute(
    input: &[u8],
    pool: &StringPool,
    attrs: usize,
    count: usize,
    attr_size: usize,
    wanted: &str,
) -> Result<Option<u32>> {
    if attr_size < 20 {
        return Err("manifest attribute record is too small".to_owned());
    }
    for index in 0..count {
        let attr = checked_add(attrs, index * attr_size, "manifest attribute")?;
        let name = pool_string(pool, u32le(input, attr + 4, "attribute name")?)?;
        if name != wanted {
            continue;
        }
        let value_size = u16le(input, attr + 12, "attribute value size")?;
        let value_type = *bytes(input, attr + 15, 1, "attribute type")?
            .first()
            .ok_or_else(|| "attribute type is absent".to_owned())?;
        return if value_size == 8 && matches!(value_type, TYPE_INT_DEC | TYPE_INT_HEX) {
            Ok(Some(u32le(input, attr + 16, "attribute integer")?))
        } else {
            Ok(None)
        };
    }
    Ok(None)
}

fn find_boolean_attribute(
    input: &[u8],
    pool: &StringPool,
    attrs: usize,
    count: usize,
    attr_size: usize,
    wanted: &str,
) -> Result<Option<bool>> {
    if attr_size < 20 {
        return Err("manifest attribute record is too small".to_owned());
    }
    for index in 0..count {
        let attr = checked_add(attrs, index * attr_size, "manifest attribute")?;
        let name = pool_string(pool, u32le(input, attr + 4, "attribute name")?)?;
        if name != wanted {
            continue;
        }
        let value_size = u16le(input, attr + 12, "attribute value size")?;
        let value_type = *bytes(input, attr + 15, 1, "attribute type")?
            .first()
            .ok_or_else(|| "attribute type is absent".to_owned())?;
        return if value_size == 8 && value_type == TYPE_INT_BOOLEAN {
            Ok(Some(u32le(input, attr + 16, "attribute boolean")? != 0))
        } else {
            Ok(None)
        };
    }
    Ok(None)
}

fn normalize_activity(package: &str, name: &str) -> Result<String> {
    let full = if name.starts_with('.') {
        format!("{package}{name}")
    } else if name.contains('.') {
        name.to_owned()
    } else {
        format!("{package}.{name}")
    };
    if full.len() > 512
        || full.split('.').any(|part| {
            part.is_empty()
                || !part
                    .bytes()
                    .all(|b| b == b'_' || b == b'$' || b.is_ascii_alphanumeric())
        })
    {
        return Err("launcher Activity has an unsupported Java name".to_owned());
    }
    Ok(full)
}

fn activity_label(activity: &str) -> String {
    activity
        .rsplit('.')
        .next()
        .unwrap_or(activity)
        .trim_start_matches('$')
        .to_owned()
}

fn parse_manifest(input: &[u8]) -> Result<ManifestInfo> {
    if u16le(input, 0, "XML type")? != RES_XML {
        return Err("AndroidManifest.xml is not binary Android XML".to_owned());
    }
    let total = u32le(input, 4, "XML size")? as usize;
    if total != input.len() {
        return Err("binary manifest size mismatch".to_owned());
    }
    let mut offset = usize::from(u16le(input, 2, "XML header size")?);
    let mut pool = None;
    let mut depth = 0_usize;
    let mut package = None;
    let mut application = None;
    let mut application_process = None;
    let mut version_code = None;
    let mut version_name = None;
    let mut label = None;
    let mut label_res = None;
    let mut application_theme = None;
    let mut target_sdk = None;
    let mut current: Option<ActivityCandidate> = None;
    let mut launchers = Vec::new();
    let mut activities = Vec::new();
    let mut activity_aliases = Vec::new();
    let mut service_names = Vec::new();
    let mut application_depth = None;
    let mut application_metadata = Vec::new();
    while offset < input.len() {
        let chunk_type = u16le(input, offset, "XML chunk type")?;
        let header_size = usize::from(u16le(input, offset + 2, "XML chunk header")?);
        let chunk_size = u32le(input, offset + 4, "XML chunk size")? as usize;
        if header_size < 8
            || chunk_size < header_size
            || checked_add(offset, chunk_size, "XML chunk")? > input.len()
        {
            return Err("binary manifest chunk bounds are invalid".to_owned());
        }
        if chunk_type == RES_STRING_POOL {
            if pool.is_some() {
                return Err("binary manifest has multiple string pools".to_owned());
            }
            pool = Some(parse_string_pool(input, offset, chunk_size)?);
        } else if chunk_type == RES_XML_START_ELEMENT {
            let strings = pool
                .as_ref()
                .ok_or_else(|| "XML node precedes string pool".to_owned())?;
            if header_size < 16 || chunk_size < 36 {
                return Err("start-element header is too small".to_owned());
            }
            let tag = pool_string(strings, u32le(input, offset + 20, "element name")?)?;
            let attr_start = usize::from(u16le(input, offset + 24, "attribute start")?);
            let attr_size = usize::from(u16le(input, offset + 26, "attribute size")?);
            let attr_count = usize::from(u16le(input, offset + 28, "attribute count")?);
            let attrs = checked_add(offset + 16, attr_start, "attribute array")?;
            let attrs_end = checked_add(attrs, attr_count * attr_size, "attribute array")?;
            if attrs_end > offset + chunk_size {
                return Err("attribute array exceeds element chunk".to_owned());
            }
            depth += 1;
            match tag {
                "manifest" => {
                    package =
                        find_attribute(input, strings, attrs, attr_count, attr_size, "package")?;
                    version_code = find_integer_attribute(
                        input,
                        strings,
                        attrs,
                        attr_count,
                        attr_size,
                        "versionCode",
                    )?;
                    version_name = find_attribute(
                        input,
                        strings,
                        attrs,
                        attr_count,
                        attr_size,
                        "versionName",
                    )?;
                }
                "uses-sdk" => {
                    target_sdk = find_integer_attribute(
                        input,
                        strings,
                        attrs,
                        attr_count,
                        attr_size,
                        "targetSdkVersion",
                    )?;
                }
                "application" => {
                    application_depth = Some(depth);
                    application =
                        find_attribute(input, strings, attrs, attr_count, attr_size, "name")?;
                    application_process =
                        find_attribute(input, strings, attrs, attr_count, attr_size, "process")?;
                    label = find_attribute(input, strings, attrs, attr_count, attr_size, "label")?;
                    label_res = find_resource_attribute(
                        input, strings, attrs, attr_count, attr_size, "label",
                    )?;
                    application_theme = find_resource_attribute(
                        input, strings, attrs, attr_count, attr_size, "theme",
                    )?;
                }
                "meta-data" if application_depth == depth.checked_sub(1) => {
                    if let (Some(name), Some(value)) = (
                        find_attribute(input, strings, attrs, attr_count, attr_size, "name")?,
                        find_metadata_value(input, strings, attrs, attr_count, attr_size)?,
                    ) {
                        application_metadata.push(ManifestMetadata { name, value });
                    }
                }
                "activity" | "activity-alias" => {
                    if current.is_some() {
                        return Err("nested activity declarations are invalid".to_owned());
                    }
                    let component_name =
                        find_attribute(input, strings, attrs, attr_count, attr_size, "name")?
                            .ok_or_else(|| format!("{tag} is missing android:name"))?;
                    let name = if tag == "activity-alias" {
                        find_attribute(
                            input,
                            strings,
                            attrs,
                            attr_count,
                            attr_size,
                            "targetActivity",
                        )?
                        .ok_or_else(|| {
                            "activity-alias is missing android:targetActivity".to_owned()
                        })?
                    } else {
                        component_name.clone()
                    };
                    let theme = find_resource_attribute(
                        input, strings, attrs, attr_count, attr_size, "theme",
                    )?;
                    let activity_label =
                        find_attribute(input, strings, attrs, attr_count, attr_size, "label")?;
                    let activity_label_res = find_resource_attribute(
                        input, strings, attrs, attr_count, attr_size, "label",
                    )?;
                    current = Some(ActivityCandidate {
                        depth,
                        component_name,
                        name,
                        theme,
                        label: activity_label,
                        label_res: activity_label_res,
                        main: false,
                        launcher: false,
                        alias: tag == "activity-alias",
                    });
                }
                "service" => {
                    let enabled = find_boolean_attribute(
                        input, strings, attrs, attr_count, attr_size, "enabled",
                    )?
                    .unwrap_or(true);
                    if enabled {
                        let name =
                            find_attribute(input, strings, attrs, attr_count, attr_size, "name")?
                                .ok_or_else(|| "service is missing android:name".to_owned())?;
                        let process = find_attribute(
                            input, strings, attrs, attr_count, attr_size, "process",
                        )?;
                        service_names.push(ServiceCandidate { name, process });
                    }
                }
                "action" if current.is_some() => {
                    if find_attribute(input, strings, attrs, attr_count, attr_size, "name")?
                        .as_deref()
                        == Some("android.intent.action.MAIN")
                    {
                        current.as_mut().expect("checked").main = true;
                    }
                }
                "category" if current.is_some() => {
                    if find_attribute(input, strings, attrs, attr_count, attr_size, "name")?
                        .as_deref()
                        == Some("android.intent.category.LAUNCHER")
                    {
                        current.as_mut().expect("checked").launcher = true;
                    }
                }
                _ => {}
            }
        } else if chunk_type == RES_XML_END_ELEMENT {
            let strings = pool
                .as_ref()
                .ok_or_else(|| "XML node precedes string pool".to_owned())?;
            let tag = pool_string(strings, u32le(input, offset + 20, "end element name")?)?;
            if tag == "activity" || tag == "activity-alias" {
                let candidate = current
                    .take()
                    .ok_or_else(|| "activity end without start".to_owned())?;
                if candidate.depth != depth {
                    return Err("activity element depth mismatch".to_owned());
                }
                if candidate.main && candidate.launcher {
                    launchers.push(candidate.clone());
                }
                if candidate.alias {
                    activity_aliases.push(candidate);
                } else {
                    activities.push(candidate);
                }
            }
            depth = depth
                .checked_sub(1)
                .ok_or_else(|| "XML depth underflow".to_owned())?;
        }
        offset += chunk_size;
    }
    let package = package.ok_or_else(|| "manifest package is missing".to_owned())?;
    let application = match application {
        Some(name) => normalize_activity(&package, &name)?,
        None => "android.app.Application".to_owned(),
    };
    if launchers.len() != 1 {
        return Err(format!(
            "APK must declare exactly one MAIN/LAUNCHER activity, found {}",
            launchers.len()
        ));
    }
    let launcher = launchers
        .pop()
        .ok_or_else(|| "launcher Activity is missing".to_owned())?;
    let activity = normalize_activity(&package, &launcher.name)?;
    let launch_component = normalize_activity(&package, &launcher.component_name)?;
    let activity_themes = activities
        .into_iter()
        .map(|candidate| {
            Ok((
                normalize_activity(&package, &candidate.name)?,
                candidate.theme.or(application_theme).unwrap_or(0),
            ))
        })
        .collect::<Result<Vec<_>>>()?;
    let activity_aliases = activity_aliases
        .into_iter()
        .map(|candidate| {
            Ok((
                normalize_activity(&package, &candidate.component_name)?,
                normalize_activity(&package, &candidate.name)?,
            ))
        })
        .collect::<Result<Vec<_>>>()?;
    let services = service_names
        .into_iter()
        .map(|service| {
            let name = normalize_activity(&package, &service.name)?;
            let declared = service
                .process
                .as_deref()
                .or(application_process.as_deref())
                .unwrap_or(&package);
            let process = if let Some(suffix) = declared.strip_prefix(':') {
                format!("{package}:{suffix}")
            } else {
                declared.to_owned()
            };
            Ok((name, process))
        })
        .collect::<Result<Vec<_>>>()?;
    // The MAIN/LAUNCHER declaration can be an alias (or a second manifest
    // occurrence) without its own theme. Android resolves the effective
    // ActivityInfo for the target activity, including the theme declared on
    // that activity, before launching it. Keep the launcher-selected class but
    // take its already-resolved theme from the activity table when available.
    let theme = activity_themes
        .iter()
        .find_map(|(name, theme)| (name == &activity).then_some(*theme))
        .or(launcher.theme)
        .or(application_theme)
        .unwrap_or(0);
    let label_res = launcher.label_res.or(label_res).unwrap_or(0);
    let label = launcher
        .label
        .or(label)
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| activity_label(&activity));
    Ok(ManifestInfo {
        package,
        application,
        activity,
        launch_component,
        activity_themes,
        activity_aliases,
        services,
        application_metadata,
        version_code: version_code.unwrap_or(0),
        version_name: version_name.unwrap_or_default(),
        theme,
        target_sdk: target_sdk.unwrap_or(1),
        label,
        label_res,
        icon: None,
    })
}

fn hex_bytes(value: &str) -> String {
    value
        .as_bytes()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

fn encode_application_metadata(entries: &[ManifestMetadata]) -> String {
    if entries.is_empty() {
        return "none".to_owned();
    }
    entries
        .iter()
        .map(|entry| {
            let (kind, data) = match &entry.value {
                ManifestMetadataValue::Resource(value) => ("r", format!("{value:08x}")),
                ManifestMetadataValue::Integer(value) => ("i", format!("{value:08x}")),
                ManifestMetadataValue::Boolean(value) => (
                    "b",
                    if *value {
                        "1".to_owned()
                    } else {
                        "0".to_owned()
                    },
                ),
                ManifestMetadataValue::String(value) => ("s", hex_bytes(value)),
            };
            format!("{}:{kind}:{data}", hex_bytes(&entry.name))
        })
        .collect::<Vec<_>>()
        .join(",")
}

fn icon_entry(entries: &[ZipEntry]) -> Option<String> {
    entries
        .iter()
        .filter_map(|entry| {
            let name = std::str::from_utf8(&entry.name).ok()?;
            let lower = name.to_ascii_lowercase();
            let is_image = lower.ends_with(".png") || lower.ends_with(".webp");
            if !is_image || (!lower.contains("icon") && !lower.contains("launcher")) {
                return None;
            }
            let density = if lower.contains("xxxhdpi") {
                5
            } else if lower.contains("xxhdpi") {
                4
            } else if lower.contains("xhdpi") {
                3
            } else if lower.contains("hdpi") {
                2
            } else if lower.contains("mdpi") {
                1
            } else {
                0
            };
            Some((density, name.len(), name.to_owned()))
        })
        .max_by_key(|(density, length, _)| (*density, *length))
        .map(|(_, _, name)| name)
}

fn validate_dex(input: &[u8]) -> Result<()> {
    if input.len() < 0x70 || &input[0..4] != b"dex\n" || input[7] != 0 {
        return Err("classes.dex has an invalid DEX magic".to_owned());
    }
    let version = &input[4..7];
    if !version.iter().all(u8::is_ascii_digit) {
        return Err("classes.dex version is invalid".to_owned());
    }
    if u32le(input, 32, "DEX file size")? as usize != input.len()
        || u32le(input, 36, "DEX header size")? != 0x70
        || u32le(input, 40, "DEX endian tag")? != 0x1234_5678
    {
        return Err("classes.dex header contract mismatch".to_owned());
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct NativeEntrySignals {
    java_exports: usize,
    jni_on_load: bool,
}

fn dynamic_jni_signals(input: &[u8]) -> Result<NativeEntrySignals> {
    if input.len() < 64
        || &input[..4] != b"\x7fELF"
        || input[4] != 2
        || input[5] != 1
        || u16le(input, 18, "ELF machine")? != 183
    {
        return Err("arm64-v8a entry is not ELF64 little-endian AArch64".to_owned());
    }
    let section_offset = usize::try_from(u64le(input, 40, "ELF section offset")?)
        .map_err(|_| "ELF section offset exceeds addressable size".to_owned())?;
    let section_size = usize::from(u16le(input, 58, "ELF section size")?);
    let section_count = usize::from(u16le(input, 60, "ELF section count")?);
    if section_offset == 0 || section_count == 0 {
        return Ok(NativeEntrySignals::default());
    }
    if section_size < 64 || section_count > 16_384 {
        return Err("ELF section table shape is invalid".to_owned());
    }
    bytes(
        input,
        section_offset,
        section_size
            .checked_mul(section_count)
            .ok_or_else(|| "ELF section table size overflows".to_owned())?,
        "ELF section table",
    )?;
    let section = |index: usize| -> Result<usize> {
        if index >= section_count {
            return Err("ELF section link is out of range".to_owned());
        }
        checked_add(section_offset, index * section_size, "ELF section")
    };
    let mut signals = NativeEntrySignals::default();
    for index in 0..section_count {
        let symbol_section = section(index)?;
        if u32le(input, symbol_section + 4, "ELF section type")? != 11 {
            continue;
        }
        let symbols_offset =
            usize::try_from(u64le(input, symbol_section + 24, "dynsym offset")?)
                .map_err(|_| "ELF dynsym offset exceeds addressable size".to_owned())?;
        let symbols_size = usize::try_from(u64le(input, symbol_section + 32, "dynsym size")?)
            .map_err(|_| "ELF dynsym size exceeds addressable size".to_owned())?;
        let symbol_size = usize::try_from(u64le(input, symbol_section + 56, "dynsym entry size")?)
            .map_err(|_| "ELF dynsym entry size exceeds addressable size".to_owned())?;
        if symbol_size < 24 || symbols_size % symbol_size != 0 {
            return Err("ELF dynamic symbol table shape is invalid".to_owned());
        }
        bytes(input, symbols_offset, symbols_size, "ELF dynamic symbols")?;
        let strings_index = u32le(input, symbol_section + 40, "dynsym string link")? as usize;
        let strings_section = section(strings_index)?;
        let strings_offset = usize::try_from(u64le(input, strings_section + 24, "dynstr offset")?)
            .map_err(|_| "ELF dynstr offset exceeds addressable size".to_owned())?;
        let strings_size = usize::try_from(u64le(input, strings_section + 32, "dynstr size")?)
            .map_err(|_| "ELF dynstr size exceeds addressable size".to_owned())?;
        let strings = bytes(input, strings_offset, strings_size, "ELF dynamic strings")?;
        for symbol in (0..symbols_size).step_by(symbol_size) {
            let record = symbols_offset + symbol;
            let name_offset = u32le(input, record, "dynamic symbol name")? as usize;
            let info = *bytes(input, record + 4, 1, "dynamic symbol info")?
                .first()
                .expect("one-byte slice");
            let section_index = u16le(input, record + 6, "dynamic symbol section")?;
            if name_offset >= strings.len() || section_index == 0 || !matches!(info >> 4, 1 | 2) {
                continue;
            }
            let tail = &strings[name_offset..];
            let length = tail
                .iter()
                .position(|byte| *byte == 0)
                .ok_or_else(|| "ELF dynamic symbol has an unterminated name".to_owned())?;
            let name = &tail[..length];
            if name == b"JNI_OnLoad" {
                signals.jni_on_load = true;
            }
            if name.starts_with(b"Java_") {
                signals.java_exports += 1;
            }
        }
    }
    Ok(signals)
}

fn select_native_root(candidates: &[(String, NativeEntrySignals)]) -> Option<String> {
    candidates
        .iter()
        .max_by_key(|(name, signals)| {
            (
                signals.java_exports > 0,
                signals.java_exports,
                signals.jni_on_load,
                name.ends_with("jni.so"),
                name.as_str(),
            )
        })
        .map(|(name, _)| name.clone())
}

type Inspection = (
    ManifestInfo,
    &'static str,
    usize,
    Vec<String>,
    Option<String>,
);

fn inspect(path: &Path, external_dex: Option<&Path>) -> Result<Inspection> {
    let metadata = fs::metadata(path).map_err(|error| format!("APK metadata failed: {error}"))?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > MAX_APK_SIZE as u64 {
        return Err(format!("APK is outside the 1..={MAX_APK_SIZE} byte cap"));
    }
    let input = fs::read(path).map_err(|error| format!("APK read failed: {error}"))?;
    let entries = parse_zip(&input)?;
    let mut native_libraries = entries
        .iter()
        .filter_map(|entry| {
            let leaf = entry.name.strip_prefix(b"lib/arm64-v8a/")?;
            if leaf.is_empty() || leaf.contains(&b'/') || !leaf.ends_with(b".so") {
                return None;
            }
            std::str::from_utf8(leaf).ok().map(str::to_owned)
        })
        .collect::<Vec<_>>();
    native_libraries.sort();
    native_libraries.dedup();
    let native_candidates = native_libraries
        .iter()
        .map(|name| {
            let archive_name = format!("lib/arm64-v8a/{name}");
            let entry = entries
                .iter()
                .find(|entry| entry.name == archive_name.as_bytes())
                .ok_or_else(|| "native APK entry disappeared during inspection".to_owned())?;
            let library = entry_bytes(&input, entry, MAX_NATIVE_LIBRARY_SIZE)?;
            Ok((name.clone(), dynamic_jni_signals(&library)?))
        })
        .collect::<Result<Vec<_>>>()?;
    let native_root = select_native_root(&native_candidates);
    let manifest_entries = entries
        .iter()
        .filter(|entry| entry.name == b"AndroidManifest.xml")
        .collect::<Vec<_>>();
    let mut dex_entries = entries
        .iter()
        .filter(|entry| {
            entry.name == b"classes.dex"
                || (entry.name.starts_with(b"classes")
                    && entry.name.ends_with(b".dex")
                    && entry.name[7..entry.name.len() - 4]
                        .iter()
                        .all(u8::is_ascii_digit))
        })
        .collect::<Vec<_>>();
    dex_entries.sort_by(|left, right| left.name.cmp(&right.name));
    if manifest_entries.len() != 1 {
        return Err("APK must contain exactly one manifest".to_owned());
    }
    let manifest = entry_bytes(&input, manifest_entries[0], MAX_MANIFEST_SIZE)?;
    let (dex, dex_source) = match external_dex {
        Some(dex_path) => {
            if !dex_entries.is_empty() {
                return Err(
                    "external DEX is only valid for a preoptimized APK without classes.dex"
                        .to_owned(),
                );
            }
            let metadata = fs::metadata(dex_path)
                .map_err(|error| format!("external DEX metadata failed: {error}"))?;
            if !metadata.is_file() || metadata.len() == 0 || metadata.len() > MAX_DEX_SIZE as u64 {
                return Err(format!(
                    "external DEX is outside the 1..={MAX_DEX_SIZE} byte cap"
                ));
            }
            (
                fs::read(dex_path).map_err(|error| format!("external DEX read failed: {error}"))?,
                "external",
            )
        }
        None => {
            if !dex_entries.iter().any(|entry| entry.name == b"classes.dex") {
                return Err(
                    "APK must contain primary classes.dex or provide an external DEX".to_owned(),
                );
            }
            (
                entry_bytes(
                    &input,
                    dex_entries
                        .iter()
                        .find(|entry| entry.name == b"classes.dex")
                        .expect("primary DEX presence was checked"),
                    MAX_DEX_SIZE,
                )?,
                "apk",
            )
        }
    };
    validate_dex(&dex)?;
    if external_dex.is_none() {
        for entry in &dex_entries {
            validate_dex(&entry_bytes(&input, entry, MAX_DEX_SIZE)?)?;
        }
    }
    let mut info = parse_manifest(&manifest)?;
    info.icon = icon_entry(&entries);
    let dex_count = if external_dex.is_some() {
        1
    } else {
        dex_entries.len()
    };
    Ok((info, dex_source, dex_count, native_libraries, native_root))
}

fn descriptor(class_name: &str) -> String {
    format!("L{};", class_name.replace('.', "/"))
}

fn run() -> Result<()> {
    let mut args = env::args_os();
    let program = args
        .next()
        .unwrap_or_else(|| "android-apk-app-runtime".into());
    let path = args
        .next()
        .ok_or_else(|| format!("usage: {} APK [APP_DEX]", Path::new(&program).display()))?;
    let external_dex = args.next();
    if args.next().is_some() {
        return Err(format!(
            "usage: {} APK [APP_DEX]",
            Path::new(&program).display()
        ));
    }
    let (info, dex_source, dex_count, native_libraries, native_root) =
        inspect(Path::new(&path), external_dex.as_deref().map(Path::new))?;
    println!(
        "apk-app-runtime: package={} application={} activity={} launch_component={} descriptor={} activities={} activity_aliases={} services={} application_metadata={} version_code={} version_name={} theme={:#x} target_sdk={} label={} label_res={:#x} icon={} dex={}-{} native={} native_root={}",
        info.package,
        info.application,
        info.activity,
        info.launch_component,
        descriptor(&info.activity),
        info.activity_themes
            .iter()
            .map(|(name, theme)| format!("{name}={theme:#x}"))
            .collect::<Vec<_>>()
            .join(","),
        if info.activity_aliases.is_empty() {
            "none".to_owned()
        } else {
            info.activity_aliases
                .iter()
                .map(|(alias, target)| format!("{alias}>{target}"))
                .collect::<Vec<_>>()
                .join(",")
        },
        if info.services.is_empty() {
            "none".to_owned()
        } else {
            info.services
                .iter()
                .map(|(name, process)| format!("{name}>{process}"))
                .collect::<Vec<_>>()
                .join(",")
        },
        encode_application_metadata(&info.application_metadata),
        info.version_code,
        info.version_name,
        info.theme,
        info.target_sdk,
        info.label,
        info.label_res,
        info.icon.as_deref().unwrap_or("none"),
        dex_source,
        dex_count,
        native_libraries.len(),
        native_root.as_deref().unwrap_or("none")
    );
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("android-apk-app-runtime: {error}");
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_root_prefers_java_jni_entry_over_runtime_and_dependency() {
        let candidates = vec![
            ("libc++_shared.so".to_owned(), NativeEntrySignals::default()),
            (
                "libvlc.so".to_owned(),
                NativeEntrySignals {
                    java_exports: 0,
                    jni_on_load: true,
                },
            ),
            (
                "libvlcjni.so".to_owned(),
                NativeEntrySignals {
                    java_exports: 240,
                    jni_on_load: false,
                },
            ),
        ];
        assert_eq!(
            select_native_root(&candidates).as_deref(),
            Some("libvlcjni.so")
        );
    }

    #[test]
    fn native_root_uses_jni_on_load_for_registered_native_library() {
        let candidates = vec![
            ("libchild.so".to_owned(), NativeEntrySignals::default()),
            (
                "libroot.so".to_owned(),
                NativeEntrySignals {
                    java_exports: 0,
                    jni_on_load: true,
                },
            ),
        ];
        assert_eq!(
            select_native_root(&candidates).as_deref(),
            Some("libroot.so")
        );
    }
}
