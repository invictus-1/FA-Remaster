// (photofix2) The graphics card's own memory as every program on the computer uses it.
//
// A program's own budget on the card (DXGI's QueryVideoMemoryInfo, Vulkan's VK_EXT_memory_budget) is what the system
// promises it; while other programs hold most of the card it still promised far more than was free (measured: 12.6 GB
// said with 1.9 GB free on the card, another program holding 14 GB), and a photo planned by it was drawn into memory
// the system had to page. The devices' MemoryBudget therefore also asks how much of the card all programs use together,
// as Task Manager shows it.
#pragma once
#include <cstdint>

namespace render::gfx {

// The card's own memory every program uses together, bytes, for the card whose adapter has the locally unique id
// `luidLow`, `luidHigh` (Windows: the "GPU Adapter Memory" performance counters' Dedicated Usage, summed over the card's
// parts; pdh.dll loaded when first asked). False where it is not known (another system, or no such counter). The first
// time some 340 ms (CardMemoryWarmUp ahead takes it on a thread of its own), after that well under one.
bool CardMemoryUsedByAll(uint32_t luidLow, int32_t luidHigh, uint64_t* used);
// Makes the counters ready on a thread of its own (once; at once where they are, or on other systems), so that the first
// CardMemoryUsedByAll does not wait: called when a photo may soon be asked for (the photo mode's calls).
void CardMemoryWarmUp();

}  // namespace render::gfx
