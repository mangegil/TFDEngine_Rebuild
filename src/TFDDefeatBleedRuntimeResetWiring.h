#pragma once

namespace TFD::DefeatBleedRuntimeResetWiring
{
	void Reset(bool preserveCaptive = false);
	void TransitionToPleasureCommit(const char* reason);
}
