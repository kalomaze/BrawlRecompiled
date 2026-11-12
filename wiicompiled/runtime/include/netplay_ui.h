#pragma once

namespace NetplayUi {

// Draw session status and process ready lobby plans inside the ImGui frame.
void Draw();

// Keep the window responsive while a network session waits for another player.
void OnSessionStall();

} // namespace NetplayUi
