#pragma once
#include "shell_tag_protocol.h"

namespace pulse::app::shell_tags {
// Call on the owning STA after its catalog is ready, and revoke before COM shutdown.
HRESULT RegisterCommandServer(const CLSID* class_override = nullptr);
void RevokeCommandServer();
std::vector<Request> TakeCommandBatches();
bool CommandServerBusy();
unsigned TakeCommandFailures();
}
