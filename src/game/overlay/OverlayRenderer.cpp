/******************************************************************************
 * Copyright (c) 2026 Sims 4 mods developers
 *****************************************************************************/

#include "game/overlay/OverlayRenderer.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <windows.h>

namespace ts4_mod::overlay
{

using Microsoft::WRL::ComPtr;

ts4::Result<std::unique_ptr<OverlayRenderer>> OverlayRenderer::Create(IDXGISwapChain* swapChain)
{
	auto renderer = std::unique_ptr<OverlayRenderer>(new OverlayRenderer());
	if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&renderer->_device))))
	{
		return ts4::MakeError("the swap chain has no Direct3D 11 device");
	}
	renderer->_device->GetImmediateContext(&renderer->_context);
	if ((renderer->_context == nullptr) || !ImGui_ImplDX11_Init(renderer->_device.Get(), renderer->_context.Get()))
	{
		return ts4::MakeError("the Direct3D 11 backend of ImGui did not start");
	}
	renderer->_started = true;
	return renderer;
}

OverlayRenderer::~OverlayRenderer()
{
	_target.Reset();
	if (_started)
	{
		ImGui_ImplDX11_Shutdown();
	}
}

void OverlayRenderer::NewFrame()
{
	ImGui_ImplDX11_NewFrame();
}

RenderResult OverlayRenderer::Render(IDXGISwapChain* swapChain, ImDrawData* drawData)
{
	if (_target == nullptr)
	{
		ComPtr<ID3D11Texture2D> buffer;
		if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
		    FAILED(_device->CreateRenderTargetView(buffer.Get(), nullptr, &_target)))
		{
			return RenderResult::Failed;
		}
	}
	// Keep the targets of the game, and set them again after the menu bar. The backend keeps the other states.
	ComPtr<ID3D11RenderTargetView> gameTarget;
	ComPtr<ID3D11DepthStencilView> gameDepth;
	_context->OMGetRenderTargets(1, &gameTarget, &gameDepth);
	ID3D11RenderTargetView* const target = _target.Get();
	_context->OMSetRenderTargets(1, &target, nullptr);
	ImGui_ImplDX11_RenderDrawData(drawData);
	ID3D11RenderTargetView* const restored = gameTarget.Get();
	_context->OMSetRenderTargets(1, &restored, gameDepth.Get());
	return RenderResult::Drawn;
}

void OverlayRenderer::ReleaseBackBuffers()
{
	_target.Reset();
}

bool IsD3D11SwapChain(IDXGISwapChain* swapChain)
{
	ComPtr<ID3D11Device> device;
	return SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)));
}

} // namespace ts4_mod::overlay
