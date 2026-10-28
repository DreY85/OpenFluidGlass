#include "pch.h"
#include "AccentOverrider.hpp"
#include "GlassKernel.hpp"
#include "uDwmProjection.hpp"
#include "dwmcoreProjection.hpp"
#include "Shared.hpp"
#include "GlassEffectBrush.hpp"
#include "GlassReflectionBrush.hpp"
#include <tlhelp32.h>

using namespace OpenGlass;
namespace OpenGlass::AccentOverrider
{
	HRESULT MyCAccent_UpdateAccentPolicy(
		uDWM::CAccent* This,
		LPCRECT rect,
		uDWM::ACCENT_POLICY* policy,
		uDWM::CBaseGeometryProxy* geometry
	);
	HRESULT MyCAccent__UpdateSolidFill(
		uDWM::CAccent* This,
		uDWM::CRenderDataVisual* visual,
		UINT color,
		const D2D1_RECT_F& rect,
		float opacity
	);
	void MyCAccent_SetClipRegion(
		uDWM::CAccent* This,
		uDWM::CBaseGeometryProxy* geometry
	);
	void MyCAccent_Destructor(
		uDWM::CAccent* This
	);
	Projection::Detour<uDWM::Symbol_CAccent_UpdateAccentPolicy, &MyCAccent_UpdateAccentPolicy> g_CAccent_UpdateAccentPolicy_Org{};
	Projection::Detour<uDWM::Symbol_CAccent__UpdateSolidFill, &MyCAccent__UpdateSolidFill> g_CAccent__UpdateSolidFill_Org{};
	Projection::Detour<uDWM::Symbol_CAccent_SetClipRegion, &MyCAccent_SetClipRegion> g_CAccent_SetClipRegion_Org{};
	Projection::Detour<uDWM::Symbol_CAccent__CAccent, &MyCAccent_Destructor> g_CAccent_Destructor_Org{};

	struct CAccentInfo
	{
		winrt::com_ptr<uDWM::CBaseGeometryProxy> clipRegion;
		winrt::com_ptr<uDWM::CRgnGeometryProxy> drawRegion;
		winrt::com_ptr<uDWM::CRenderDataVisual> visual;
	};
	std::unordered_map<uDWM::CAccent*, CAccentInfo> g_accentClipRegions{};

	// Our own flag, unused by Windows: the window asks for its glass to be a pill, cut to an
	// outline rounded by half its height. It is removed before the policy reaches uDWM's own accent code.
	constexpr DWORD AccentFlagPill{ 0x80000000 };

	// Apps that keep their own accent blur while the override is on. Their UI is drawn over a tinted
	// acrylic, and over clear Fluid Glass it can't be read (ESET's egui.exe). GlassOverrideAccentExclusions
	// lists executable names separated by semicolons; without the value, ESET is excluded.
	constexpr PCWSTR c_exclusionsValue{ L"GlassOverrideAccentExclusions" };
	constexpr PCWSTR c_defaultExclusions{ L"egui.exe" };
	std::vector<std::wstring> g_exclusions{};
	std::unordered_map<DWORD, bool> g_processExcluded{};	// by process id, cleared whenever the list is read

	void LoadExclusions()
	{
		WCHAR value[2048]{};
		if (
			FAILED(wil::reg::get_value_string_nothrow(GlassEngine::GetDwmKey(), c_exclusionsValue, value)) &&
			FAILED(wil::reg::get_value_string_nothrow(GlassEngine::GetMachineDwmKey(), c_exclusionsValue, value))
		)
		{
			wcscpy_s(value, c_defaultExclusions);
		}
		g_exclusions.clear();
		g_processExcluded.clear();
		PWSTR context{};
		for (PWSTR name = wcstok_s(value, L";\r\n", &context); name; name = wcstok_s(nullptr, L";\r\n", &context))
		{
			std::wstring_view entry{ name };
			while (!entry.empty() && iswspace(entry.front())) entry.remove_prefix(1);
			while (!entry.empty() && iswspace(entry.back())) entry.remove_suffix(1);
			if (!entry.empty()) g_exclusions.emplace_back(entry);
		}
	}

	// Looked up once per process. A process snapshot needs no access to the process itself,
	// which DWM's account doesn't have for every app.
	bool IsExcluded(uDWM::CTopLevelWindow* window)
	{
		if (g_exclusions.empty() || !window) return false;
		const auto data = window->GetData();
		DWORD processId{};
		if (!data || !GetWindowThreadProcessId(data->GetHwnd(), &processId)) return false;
		if (const auto it = g_processExcluded.find(processId); it != g_processExcluded.end()) return it->second;

		bool excluded{ false };
		if (const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W entry{ sizeof(entry) };
			for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
			{
				if (entry.th32ProcessID != processId) continue;
				excluded = std::any_of(g_exclusions.begin(), g_exclusions.end(), [&entry](const std::wstring& name) { return !_wcsicmp(name.c_str(), entry.szExeFile); });
				break;
			}
			CloseHandle(snapshot);
		}
		g_processExcluded.emplace(processId, excluded);
		return excluded;
	}
}

HRESULT AccentOverrider::MyCAccent_UpdateAccentPolicy(
	uDWM::CAccent* This,
	LPCRECT rect,
	uDWM::ACCENT_POLICY* policy,
	uDWM::CBaseGeometryProxy* geometry
)
{
	if (!Shared::g_overrideAccent)
	{
		return g_CAccent_UpdateAccentPolicy_Org(This, rect, policy, geometry);
	}

	if (
		(policy->AccentState != 3 &&
		policy->AccentState != 4) ||
		IsExcluded(uDWM::TryGetWindowFromVisual(This))
	)
	{
		return g_CAccent_UpdateAccentPolicy_Org(This, rect, policy, geometry);
	}

	auto accentPolicy = *policy;
	accentPolicy.AccentState = 1;
	accentPolicy.dwGradientColor = 0;
	accentPolicy.AccentFlags &= ~AccentFlagPill;
	if (const auto it = g_accentClipRegions.find(This); it != g_accentClipRegions.end() && it->second.clipRegion)
	{
		// remove accent border atlas
		accentPolicy.AccentFlags &= ~((1 << 5) | (1 << 6) | (1 << 7) | (1 << 8));
	}

	return g_CAccent_UpdateAccentPolicy_Org(This, rect, &accentPolicy, geometry);
}
HRESULT AccentOverrider::MyCAccent__UpdateSolidFill(
	uDWM::CAccent* This,
	uDWM::CRenderDataVisual* visual,
	UINT color,
	const D2D1_RECT_F& rect,
	float opacity
)
{
	const auto result = g_CAccent__UpdateSolidFill_Org(
		This,
		visual,
		color,
		rect,
		opacity
	);

	if (!Shared::g_overrideAccent)
	{
		return result;
	}

	const auto& accentPolicy = This->GetAccentPolicy();
	if (accentPolicy.AccentState == 2)
	{
		return result;
	}

	const auto window = uDWM::TryGetWindowFromVisual(This);
	if (!window || IsExcluded(window))
	{
		return result;
	}

	auto data = window->GetData();
	if (!data)
	{
		return result;
	}

	if (data->GetAccentPolicy()->AccentState == 1)
	{
		return result;
	}

	auto& accentInfo = g_accentClipRegions[This];
	accentInfo.visual.copy_from(visual);

	// indicates window should transition on maximized, so we can receive colorization updates
	// this make permanent change to the attribute during its lifetime, until a update to the attribute is made
	// is there any better way to implement it?
	// Fluid Glass stays clear while another window is maximized: the Vista transition would
	// turn the glass opaque black, so it neither asks for the transition nor follows it.
	if (!Shared::g_fluidGlass.enabled)
	{
		data->GetClientBlurAttributeReference() |= 8;
	}

	winrt::com_ptr<uDWM::CSolidColorLegacyMilBrushProxy> effectBrush{ nullptr };
	const auto maximized = !Shared::g_fluidGlass.enabled && window->TreatAsMaximized(data);

	if (!visual->GetInstructions().views().empty())
	{
		winrt::com_ptr<uDWM::CDrawGeometryInstruction> instruction{ nullptr };
		{
			const auto solidRectangleInstruction = reinterpret_cast<uDWM::CSolidRectangleInstruction*>(visual->GetInstructions().views().front());
			const auto drawRect = RectF::ToRectL(solidRectangleInstruction->GetRectangle());

			RETURN_IF_FAILED(
				uDWM::ResourceHelper::CreateGeometryFromHRGN(
					wil::unique_hrgn{ CreateRectRgnIndirect(&drawRect) }.get(),
					accentInfo.drawRegion.put()
				)
			);
			RETURN_IF_FAILED(
				uDWM::CDesktopManager::GetInstance()->GetCompositor()->CreateSolidColorLegacyMilBrushProxy(
					effectBrush.put()
				)
			);
			const bool light = GlassKernel::IsLightWindow(data);
			auto glassColor = Color::sRGBToscRGB(
				GlassKernel::RealizeWindowColorization(
					GlassKernel::GetBaseColor(Shared::IsTransparencyDisabled(), maximized),
					GlassKernel::GetSourceColor(true, light),
					GlassKernel::GetColorizationOpacity(true, maximized),
					Shared::IsTransparencyDisabled(),
					false,
					light
				).GetEffectivescRGBBlendColor(0.f),
				0.f
			);
			GlassKernel::AlphaChannelReinterpreter marker{ true, maximized };
			marker.SetIsPill((data->GetAccentPolicy()->AccentFlags & AccentFlagPill) != 0);
			marker.SetIsLight(light);
			glassColor.a = marker.ToFloat();
			RETURN_IF_FAILED(effectBrush->Update(1.0, glassColor));

			RETURN_IF_FAILED(
				uDWM::CDrawGeometryInstruction::Create(
					effectBrush.get(),
					accentInfo.drawRegion.get(),
					instruction.put()
				)
			);
			RETURN_IF_FAILED(visual->ClearInstructions());
			RETURN_IF_FAILED(visual->AddInstruction(instruction.get()));
		}

		if (
			const auto brush = GlassReflectionBrush::GetOrCreate(
				window,
				2,
				true
			);
			brush
		)
		{
			RETURN_IF_FAILED(
				brush->Update(
					(Shared::g_reflectionPolicy & Shared::ReflectionPolicy::NonClient) ?
					GlassKernel::GetAdjustedReflectionIntensity(true, maximized) :
					0.f,
					GlassReflectionBrush::CalculateTargetViewport(
						visual->GetLocalToParentVisualOffset(window->GetTransformParent()),
						Shared::g_reflectionParallaxIntensity,
						window->IsRTLMirrored(),
						window->GetWidth(),
						visual->GetScale()
					),
					D2D1::RectF(),
					nullptr,
					DWM::MilBrushMappingMode::Absolute,
					DWM::MilBrushMappingMode::Absolute,
					nullptr,
					nullptr,
					DWM::MilStretch::None,
					DWM::MilTileMode::Extend,
					DWM::MilHorizontalAlignment::Left,
					DWM::MilVerticalAlignment::Top,
					nullptr
				)
			);
			RETURN_IF_FAILED(
				uDWM::CDrawGeometryInstruction::Create(
					brush.get(),
					accentInfo.drawRegion.get(),
					instruction.put()
				)
			);
			RETURN_IF_FAILED(visual->AddInstruction(instruction.get()));
		}
	}

	if (
		accentPolicy.IsClipRegionEffective() &&
		accentInfo.clipRegion
	)
	{
		for (auto& instruction : accentInfo.visual->GetInstructions().views())
		{
			auto& originalGeometry = reinterpret_cast<uDWM::CDrawGeometryInstruction*>(instruction)->GetGeometry();
			if (originalGeometry)
			{
				originalGeometry->Release();
				originalGeometry = nullptr;
			}
			if (accentInfo.clipRegion)
			{
				winrt::com_ptr<uDWM::CCombinedGeometryProxy> combinedGeometry{ nullptr };
				RETURN_IF_FAILED(
					uDWM::ResourceHelper::CreateCombinedGeometry(
						accentInfo.clipRegion.get(),
						accentInfo.drawRegion.get(),
						D2D1_COMBINE_MODE_INTERSECT,
						combinedGeometry.put()
					)
				);
				originalGeometry = combinedGeometry.detach();
			}
			else
			{
				originalGeometry = accentInfo.drawRegion.get();
				originalGeometry->AddRef();
			}
		}
	}

	return result;
}
void AccentOverrider::MyCAccent_SetClipRegion(
	uDWM::CAccent* This,
	uDWM::CBaseGeometryProxy* geometry
)
{
	if (!Shared::g_overrideAccent)
	{
		return g_CAccent_SetClipRegion_Org(This, geometry);
	}

	const auto& accentPolicy = This->GetAccentPolicy();
	if (accentPolicy.AccentState == 2)
	{
		return g_CAccent_SetClipRegion_Org(This, geometry);
	}

	const auto window = uDWM::TryGetWindowFromVisual(This);
	if (!window || IsExcluded(window))
	{
		return g_CAccent_SetClipRegion_Org(This, geometry);
	}

	auto data = window->GetData();
	if (!data)
	{
		return g_CAccent_SetClipRegion_Org(This, geometry);
	}

	if (data->GetAccentPolicy()->AccentState == 1)
	{
		return g_CAccent_SetClipRegion_Org(This, geometry);
	}

	auto& accentInfo = g_accentClipRegions[This];
	accentInfo.clipRegion.copy_from(geometry);

	if (
		accentPolicy.IsClipRegionEffective() &&
		accentInfo.visual &&
		accentInfo.drawRegion
	)
	{
		for (auto& instruction : accentInfo.visual->GetInstructions().views())
		{
			auto& originalGeometry = reinterpret_cast<uDWM::CDrawGeometryInstruction*>(instruction)->GetGeometry();
			if (originalGeometry)
			{
				originalGeometry->Release();
				originalGeometry = nullptr;
			}
			if (accentInfo.clipRegion)
			{
				winrt::com_ptr<uDWM::CCombinedGeometryProxy> combinedGeometry{ nullptr };
				THROW_IF_FAILED(
					uDWM::ResourceHelper::CreateCombinedGeometry(
						accentInfo.clipRegion.get(),
						accentInfo.drawRegion.get(),
						D2D1_COMBINE_MODE_INTERSECT,
						combinedGeometry.put()
					)
				);
				originalGeometry = combinedGeometry.detach();
			}
			else
			{
				originalGeometry = accentInfo.drawRegion.get();
				originalGeometry->AddRef();
			}
			THROW_IF_FAILED(accentInfo.visual->UpdateRenderData());
		}
	}
	return g_CAccent_SetClipRegion_Org(This, nullptr);
}
void AccentOverrider::MyCAccent_Destructor(
	uDWM::CAccent* This
)
{
	g_accentClipRegions.erase(This);
	return g_CAccent_Destructor_Org(This);
}
void AccentOverrider::Update(GlassEngine::UpdateType type)
{
	if (type & GlassEngine::UpdateType::Backdrop)
	{
		Shared::g_overrideAccent = static_cast<bool>(GlassEngine::GetDwordFromRegistry(L"GlassOverrideAccent"));
		LoadExclusions();
		if (!Shared::g_overrideAccent)
		{
			g_accentClipRegions.clear();
		}
	}
}

void AccentOverrider::Startup()
{
	if (Shared::g_disabledHooks.test(Shared::DisabledHooks_AccentOverrider))
	{
		return;
	}

	HookHelper::ApplyInlineHooks(
		std::initializer_list<HookHelper::DetourInfo>
		{
			{ &g_CAccent_UpdateAccentPolicy_Org },
			{ &g_CAccent__UpdateSolidFill_Org },
			{ &g_CAccent_SetClipRegion_Org },
			{ &g_CAccent_Destructor_Org },
		}
	);
}

void AccentOverrider::Shutdown()
{
	if (Shared::g_disabledHooks.test(Shared::DisabledHooks_AccentOverrider))
	{
		return;
	}

	HookHelper::ApplyInlineHooks(
		std::initializer_list<HookHelper::DetourInfo>
		{
			{ &g_CAccent_UpdateAccentPolicy_Org },
			{ &g_CAccent__UpdateSolidFill_Org },
			{ &g_CAccent_SetClipRegion_Org },
			{ &g_CAccent_Destructor_Org },
		}
	);

}

void AccentOverrider::Cleanup()
{
	g_accentClipRegions.clear();
}
