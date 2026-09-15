# Regenerate from the vendored data.cpp at the revision in README.pulse.txt.
# No network access or build-time generation is required.
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'data.cpp'))
$source = $source.Replace('#include <IbPinyin/pinyin.hpp>', '')
$source = $source.Replace('#define P(s) {IB_PINYIN_LITERAL(#s)},', '#define P(s) #s,')
$source = $source.Replace('Pinyin pinyins[1514]', 'constexpr std::string_view pinyins[1514]')
$source = $source.Replace('PinyinCombination<10>', 'const PinyinCombination<10>')
$source = $source.Replace('uint16_t pinyin_table', 'const uint16_t pinyin_table')
$source = $source.Replace('PinyinRange pinyin_ranges', 'const PinyinRange pinyin_ranges')
$source = [regex]::Replace($source, 'P\(([^)]*)\)', {
    param($match)
    if ($match.Groups[1].Value -eq 's') { return $match.Value }
    $syllable = $match.Groups[1].Value.Replace('ü', 'v').Replace('ǖ', 'v').Replace('ǘ', 'v').Replace('ǚ', 'v').Replace('ǜ', 'v')
    $syllable = $syllable.Normalize([Text.NormalizationForm]::FormD)
    'P(' + [regex]::Replace($syllable, '\p{Mn}', '') + ')'
})
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'ascii_data.inc'), $source)
