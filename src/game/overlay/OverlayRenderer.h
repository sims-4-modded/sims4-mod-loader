/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#pragma once

#include <memory>

#include <common/Result.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

struct ImDrawData;

namespace ts4_mod::overlay
{

/// What one frame of the renderer did.
enum class RenderResult
{
	Drawn,  ///< The menu bar is on the back buffer.
	Failed, ///< A call failed. The menu bar stops.
};

/// Draws the Dear ImGui data on the back buffer of the swap chain of the game, with Direct3D 11 on the immediate
/// context of the game. The thread that presents calls every function, with the ImGui context current, immediately
/// before `Present`.
class OverlayRenderer
{
public:
	/// Make the renderer on the Direct3D 11 device of the swap chain. Return an error if the swap chain has no
	/// Direct3D 11 device. The ImGui context must be current.
	[[nodiscard]] static ts4::Result<std::unique_ptr<OverlayRenderer>> Create(IDXGISwapChain* swapChain);

	~OverlayRenderer();
	OverlayRenderer(const OverlayRenderer&) = delete;
	OverlayRenderer& operator=(const OverlayRenderer&) = delete;

	/// Start a frame of the renderer backend of ImGui.
	void NewFrame();
	/// Draw the frame on the current back buffer.
	[[nodiscard]] RenderResult Render(IDXGISwapChain* swapChain, ImDrawData* drawData);
	/// Release each reference to the back buffers, before the game changes their size.
	void ReleaseBackBuffers();

private:
	OverlayRenderer() = default;

	Microsoft::WRL::ComPtr<ID3D11Device> _device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _target;
	bool _started = false;
};

/// True if the device of the swap chain is a Direct3D 11 device.
[[nodiscard]] bool IsD3D11SwapChain(IDXGISwapChain* swapChain);

} // namespace ts4_mod::overlay
