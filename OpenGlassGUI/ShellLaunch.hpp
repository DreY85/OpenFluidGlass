#pragma once
#include "pch.h"
#include <exdisp.h>
#include <shldisp.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

namespace OpenGlass
{
	// This program runs elevated, but what it starts here must not: explorer could not post to an elevated window,
	// and an explorer started by an elevated process would run elevated too.
	// So explorer starts it, through the desktop's shell automation object, as the signed-in user.
	inline HRESULT StartAsShellUser(const std::wstring& path, int show = SW_SHOWNORMAL)
	{
		wil::com_ptr_nothrow<IShellWindows> windows;
		RETURN_IF_FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put())));
		VARIANT location{};
		location.vt = VT_I4;
		location.lVal = CSIDL_DESKTOP;
		VARIANT empty{};
		long window{};
		wil::com_ptr_nothrow<IDispatch> desktop;
		RETURN_IF_FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, desktop.put()));
		RETURN_HR_IF_NULL(E_FAIL, desktop);
		wil::com_ptr_nothrow<IShellBrowser> browser;
		RETURN_IF_FAILED(IUnknown_QueryService(desktop.get(), SID_STopLevelBrowser, IID_PPV_ARGS(browser.put())));
		wil::com_ptr_nothrow<IShellView> view;
		RETURN_IF_FAILED(browser->QueryActiveShellView(view.put()));
		wil::com_ptr_nothrow<IDispatch> background;
		RETURN_IF_FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.put())));
		wil::com_ptr_nothrow<IShellFolderViewDual> folderView;
		RETURN_IF_FAILED(background->QueryInterface(IID_PPV_ARGS(folderView.put())));
		wil::com_ptr_nothrow<IDispatch> application;
		RETURN_IF_FAILED(folderView->get_Application(application.put()));
		wil::com_ptr_nothrow<IShellDispatch2> shell;
		RETURN_IF_FAILED(application->QueryInterface(IID_PPV_ARGS(shell.put())));
		wil::unique_bstr file{ SysAllocString(path.c_str()) };
		RETURN_IF_NULL_ALLOC(file);
		VARIANT showWindow{};
		showWindow.vt = VT_I4;
		showWindow.lVal = show;
		return shell->ShellExecute(file.get(), empty, empty, empty, showWindow);
	}
}
