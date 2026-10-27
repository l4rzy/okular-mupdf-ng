//! EPUB format support - pure parsing functions.

mod parser;

pub use parser::{parse_container_xml, parse_nav_landmarks, parse_nav_toc, parse_ncx, parse_opf};
