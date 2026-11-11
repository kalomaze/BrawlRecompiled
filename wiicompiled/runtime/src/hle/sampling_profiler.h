#pragma once

// BRAWL_PROFILE=all samples process threads; reports accompany NSMBW_LOG_FPS windows.
// Mutually exclusive with the timed game-thread sampler in guest_profiler.cpp.
void SamplingProfilerAttachCurrentThread();
void SamplingProfilerReport();
