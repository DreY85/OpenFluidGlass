#pragma once
#include "framework.hpp"
#include "cpprt.hpp"
#include "GlassEngine.hpp"
#include "dwmcoreProjection.hpp"

namespace OpenGlass::GlassIntegrity
{
	void DestroyDeviceResources(dwmcore::CD2DContext* d2dContext);
	// Remembers where pill glass was drawn, in device pixels, for two seconds.
	// The safety zone stays on for small redraws (a hover on a taskbar button) that touch it.
	void RememberDrawnPillGlass(const D2D1_RECT_F& deviceBounds);

	void Update(GlassEngine::UpdateType type);
	void Startup();
	void Shutdown();
	void Cleanup();
}
