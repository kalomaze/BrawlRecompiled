#pragma once
namespace FrameInput { struct GcPad; }
namespace BrawlOnline {
void Loop(bool stageSeen);
void Stage();
void Poll();
void WaitForGo();
void SamplePads(FrameInput::GcPad* pads);
}
