#pragma once
#ifdef PULSE_PREVIEW_IMAGE_CACHE_RACE_TEST
namespace pulse::preview {
// Called only by the dedicated executable; all filesystem operations stay real.
void PreviewImageCacheTestStage(unsigned stage); // 1: opened; 2: closed, before rename
}
#endif
