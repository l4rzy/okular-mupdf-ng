// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

use boko::{Book, Format};
use std::fs::File;
use std::io::{self, Seek, SeekFrom, Write};
use std::os::fd::BorrowedFd;
use std::os::unix::fs::FileExt;
use std::panic::catch_unwind;

const MAX_INPUT: u64 = 128 * 1024 * 1024;
const MAX_OUTPUT: u64 = 256 * 1024 * 1024;

struct LimitedFile(File);

impl Write for LimitedFile {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let position = self.0.stream_position()?;
        if buf.len() as u64 > MAX_OUTPUT.saturating_sub(position) {
            return Err(io::Error::other("converted EPUB exceeds size limit"));
        }
        self.0.write(buf)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.0.flush()
    }
}

impl Seek for LimitedFile {
    fn seek(&mut self, pos: SeekFrom) -> io::Result<u64> {
        let current = self.0.stream_position()?;
        let end = self.0.metadata()?.len();
        let target = match pos {
            SeekFrom::Start(n) => i128::from(n),
            SeekFrom::End(n) => i128::from(end) + i128::from(n),
            SeekFrom::Current(n) => i128::from(current) + i128::from(n),
        };
        if target < 0 || target > i128::from(MAX_OUTPUT) {
            return Err(io::Error::other("converted EPUB exceeds size limit"));
        }
        self.0.seek(SeekFrom::Start(target as u64))
    }
}

// The caller retains ownership of both descriptors.
#[unsafe(no_mangle)]
pub extern "C" fn mupdfng_kindle_convert(input: i32, output: i32) -> i32 {
    if input < 0 || output < 0 {
        return -1;
    }
    let result = catch_unwind(|| -> Result<(), Box<dyn std::error::Error>> {
        // SAFETY: the caller supplies live descriptors for the duration of the call.
        let input = unsafe { BorrowedFd::borrow_raw(input) }.try_clone_to_owned()?;
        let output = unsafe { BorrowedFd::borrow_raw(output) }.try_clone_to_owned()?;
        let input = File::from(input);
        let size = input.metadata()?.len();
        if size > MAX_INPUT {
            return Err("Kindle book exceeds size limit".into());
        }
        let mut bytes = vec![0; size as usize];
        input.read_exact_at(&mut bytes, 0)?;
        let format = if bytes.starts_with(b"CONT") {
            Format::Kfx
        } else {
            Format::Azw3
        };
        let book = Book::from_bytes(&bytes, format)?;
        let mut output = LimitedFile(File::from(output));
        book.export(Format::Epub, &mut output)?;
        Ok(())
    });
    if matches!(result, Ok(Ok(()))) { 0 } else { -1 }
}

#[cfg(test)]
mod tests {
    use super::*;
    use quick_xml::{Reader, events::Event};
    use std::collections::{HashMap, HashSet};
    use std::io::{Cursor, Read};
    use std::path::{Component, Path, PathBuf};

    fn attributes(xml: &[u8]) -> Vec<(String, String)> {
        let mut reader = Reader::from_reader(xml);
        let mut result = Vec::new();
        loop {
            match reader.read_event().unwrap() {
                Event::Start(tag) | Event::Empty(tag) => {
                    for attribute in tag.attributes() {
                        let attribute = attribute.unwrap();
                        result.push((
                            String::from_utf8(attribute.key.as_ref().to_vec()).unwrap(),
                            attribute.unescape_value().unwrap().into_owned(),
                        ));
                    }
                }
                Event::Eof => break,
                _ => {}
            }
        }
        result
    }

    #[test]
    fn azw3_export_keeps_stylesheets_fonts_and_link_destinations() {
        let epub = include_bytes!("../../../../../tests/epub/styled-linked.epub");
        let original = Book::from_bytes(epub, Format::Epub).unwrap();
        let mut generated = Cursor::new(Vec::new());
        original.export(Format::Azw3, &mut generated).unwrap();
        for bytes in [
            include_bytes!("../../../../../tests/mobi/styled-linked.azw3").as_slice(),
            generated.get_ref(),
        ] {
            check_export(bytes);
        }
    }

    fn check_export(bytes: &[u8]) {
        let book = Book::from_bytes(bytes, Format::Azw3).unwrap();
        let mut output = Cursor::new(Vec::new());
        book.export(Format::Epub, &mut output).unwrap();
        let mut zip = zip::ZipArchive::new(output).unwrap();
        let mut entries = HashMap::new();
        for i in 0..zip.len() {
            let mut entry = zip.by_index(i).unwrap();
            let mut content = Vec::new();
            entry.read_to_end(&mut content).unwrap();
            entries.insert(entry.name().to_string(), content);
        }
        let mut ids = HashMap::new();
        for (path, content) in &entries {
            if path.ends_with(".html") || path.ends_with(".xhtml") {
                ids.insert(
                    path,
                    attributes(content)
                        .into_iter()
                        .filter(|(key, _)| key == "id" || key == "name")
                        .map(|(_, value)| value)
                        .collect::<HashSet<_>>(),
                );
            }
        }
        let mut stylesheet_links = 0;
        let mut fragment_links = 0;
        let mut styled_text = false;
        let mut embedded_font = false;
        for (path, content) in &entries {
            if path.ends_with(".css") {
                let css = String::from_utf8(content.clone()).unwrap();
                styled_text |= css.to_ascii_lowercase().contains("#b12345");
                embedded_font |= css.contains("../fonts/font_");
            }
            if !path.ends_with(".html") && !path.ends_with(".xhtml") && !path.ends_with(".ncx") {
                continue;
            }
            for (key, value) in attributes(content) {
                if key != "href" && key != "src" || value.contains(':') {
                    continue;
                }
                let (target, fragment) = value.split_once('#').unwrap_or((&value, ""));
                let joined = if target.is_empty() {
                    PathBuf::from(path)
                } else {
                    Path::new(path).parent().unwrap().join(target)
                };
                let mut resolved = PathBuf::new();
                for component in joined.components() {
                    match component {
                        Component::ParentDir => {
                            resolved.pop();
                        }
                        Component::CurDir => {}
                        _ => resolved.push(component),
                    }
                }
                let resolved = resolved.to_str().unwrap().to_string();
                assert!(
                    entries.contains_key(&resolved),
                    "missing resource {path}: {value}"
                );
                if target.ends_with(".css") {
                    stylesheet_links += 1;
                }
                if !fragment.is_empty() {
                    assert!(
                        ids.get(&resolved).unwrap().contains(fragment),
                        "missing anchor {path}: {value}"
                    );
                    fragment_links += 1;
                }
            }
        }
        assert!(stylesheet_links > 0);
        assert!(fragment_links > 0);
        assert!(styled_text);
        assert!(embedded_font);
        assert!(entries.keys().any(|path| path.starts_with("OEBPS/fonts/")));
    }
}
