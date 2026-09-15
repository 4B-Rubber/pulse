Pinned source: https://github.com/Chaoses-Ib/ib-matcher
Revision: 70673969ca837e6eb6caf4e2d22d494a707a5846
Files data.cpp and pinyin.hpp are unmodified upstream references, licensed under LICENSE.txt (MIT).
ascii_data.inc is derived from data.cpp: tones removed by Unicode NFD, umlaut-u spelled v, immutable arrays and string_view syllables. Only this generated data is compiled; no Rust runtime or upstream matcher is linked.
