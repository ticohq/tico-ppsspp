#include "tico/TicoGraphicsHost.h"

#if PPSSPP_PLATFORM(SWITCH)

#include "Common/CommonTypes.h"

#include <cstdint>

#include "Common/Log.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/System/Display.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "GPU/GPUState.h"
#include "GPU/Vulkan/VulkanUtil.h"

namespace {

VkExtent2D GetSwitchWindowExtent() {
	u32 width = 0;
	u32 height = 0;
	if (R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &width, &height)) && width > 0 && height > 0) {
		return VkExtent2D{ width, height };
	}
	return VkExtent2D{ 1, 1 };
}

VkPresentModeKHR GetSwitchPresentMode(Draw::DrawContext *draw) {
	const VkPresentModeKHR preferred = ConfigPresentModeToVulkan(draw);
	if (preferred != VK_PRESENT_MODE_FIFO_KHR) {
		INFO_LOG(Log::G3D, "Switch Vulkan forcing FIFO present mode (preferred=%d)", (int)preferred);
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

class TicoVulkanGraphicsContext final : public GraphicsContext {
public:
	~TicoVulkanGraphicsContext() override {
		Shutdown();
	}

	bool InitFromRenderThread(std::string *errorMessage) override {
		auto fail = [&](const std::string &message) {
			ERROR_LOG(Log::G3D, "TicoVulkanGraphicsContext init failed: %s", message.c_str());
			if (errorMessage) {
				*errorMessage = message;
			}
			Shutdown();
			return false;
		};

		INFO_LOG(Log::G3D, "TicoVulkanGraphicsContext::InitFromRenderThread begin");
		init_glslang();
		glslangInitialized_ = true;

		std::string errorStr;
		if (!VulkanLoad(&errorStr)) {
			return fail(errorStr.empty() ? "Failed to load Vulkan base functions" : errorStr);
		}

		vulkan_ = new VulkanContext();

		VulkanContext::CreateInfo info{};
		InitVulkanCreateInfoFromConfig(&info);
		if (vulkan_->CreateInstance(info) != VK_SUCCESS) {
			return fail(vulkan_->InitError());
		}

		int deviceNum = vulkan_->GetPhysicalDeviceByName(g_Config.sVulkanDevice);
		if (deviceNum < 0) {
			deviceNum = vulkan_->GetBestPhysicalDevice();
			if (deviceNum >= 0 && !g_Config.sVulkanDevice.empty()) {
				g_Config.sVulkanDevice = vulkan_->GetPhysicalDeviceProperties(deviceNum).properties.deviceName;
			}
		}
		if (deviceNum < 0) {
			return fail("No suitable Vulkan physical device found");
		}

		if (vulkan_->CreateDevice(deviceNum) != VK_SUCCESS) {
			return fail(vulkan_->InitError());
		}
		INFO_LOG(Log::G3D, "Switch Vulkan device: %s", vulkan_->GetPhysicalDeviceProperties(deviceNum).properties.deviceName);

		vulkan_->SetCbGetDrawSize([]() {
			return GetSwitchWindowExtent();
		});

		if (vulkan_->InitSurface(WINDOWSYSTEM_SWITCH, nwindowGetDefault(), nullptr) != VK_SUCCESS) {
			return fail(vulkan_->InitError());
		}
		INFO_LOG(Log::G3D, "Switch Vulkan surface initialized");

		bool useMultiThreading = g_Config.bRenderMultiThreading;
		if (g_Config.iInflightFrames == 1) {
			useMultiThreading = false;
		}

		draw_ = Draw::T3DCreateVulkanContext(vulkan_, useMultiThreading);
		if (!draw_) {
			return fail("Failed to create Vulkan draw context");
		}

		const VkPresentModeKHR presentMode = GetSwitchPresentMode(draw_);
		if (!vulkan_->InitSwapchain(presentMode)) {
			return fail(vulkan_->InitError());
		}
		INFO_LOG(Log::G3D, "Tico Vulkan swapchain initialized: %dx%d", vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());

		SetGPUBackend(GPUBackend::VULKAN, vulkan_->GetPhysicalDeviceProperties(deviceNum).properties.deviceName);

		if (!draw_->CreatePresets()) {
			return fail("Failed to create Vulkan preset shaders");
		}

		draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
		renderManager_ = reinterpret_cast<VulkanRenderManager *>(draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER));
		if (!renderManager_) {
			return fail("Failed to initialize the Vulkan render manager");
		}
		renderManager_->SetInflightFrames(g_Config.iInflightFrames);
		if (!renderManager_->HasBackbuffers()) {
			return fail("Vulkan swapchain did not expose any backbuffers");
		}

		INFO_LOG(Log::G3D, "TicoVulkanGraphicsContext::InitFromRenderThread complete");
		return true;
	}

	void Shutdown() override {
		renderManager_ = nullptr;

		if (draw_ && vulkan_ && vulkan_->IsSwapchainInited()) {
			draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
		}

		delete draw_;
		draw_ = nullptr;

		if (vulkan_) {
			if (vulkan_->GetDevice() != VK_NULL_HANDLE) {
				vulkan_->WaitUntilQueueIdle();
			}
			if (vulkan_->IsSwapchainInited()) {
				vulkan_->DestroySwapchain();
			}
			if (vulkan_->GetInstance() != VK_NULL_HANDLE) {
				vulkan_->DestroySurface();
				vulkan_->DestroyDevice();
				vulkan_->DestroyInstance();
			}
			delete vulkan_;
			vulkan_ = nullptr;
		}

		if (glslangInitialized_) {
			finalize_glslang();
			glslangInitialized_ = false;
		}
	}

	void Resize() override {
		if (!draw_ || !vulkan_ || !vulkan_->IsSwapchainInited()) {
			return;
		}

		INFO_LOG(Log::G3D, "TicoVulkanGraphicsContext::Resize old=%dx%d", vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
		draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
		vulkan_->DestroySwapchain();

		const VkPresentModeKHR presentMode = GetSwitchPresentMode(draw_);
		if (vulkan_->InitSwapchain(presentMode)) {
			draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
			INFO_LOG(Log::G3D, "TicoVulkanGraphicsContext::Resize new=%dx%d", vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
		} else {
			ERROR_LOG(Log::G3D, "TicoVulkanGraphicsContext::Resize failed: %s", vulkan_->InitError().c_str());
		}
	}

	void Poll() override {
		if (!draw_ || !vulkan_ || !renderManager_ || !vulkan_->IsSwapchainInited()) {
			return;
		}

		if (renderManager_->NeedsSwapchainRecreate()) {
			INFO_LOG(Log::G3D, "TicoVulkanGraphicsContext::Poll requested swapchain recreate");
			Resize();
		}
	}

	Draw::DrawContext *GetDrawContext() override {
		return draw_;
	}

	void *GetAPIContext() override {
		return vulkan_;
	}

private:
	Draw::DrawContext *draw_ = nullptr;
	VulkanRenderManager *renderManager_ = nullptr;
	VulkanContext *vulkan_ = nullptr;
	bool glslangInitialized_ = false;
};

}  // namespace

bool TicoGraphicsHost::Init(std::string *error_message, GraphicsContext **ctx, GPUCore core) {
	if (core != GPUCORE_VULKAN) {
		if (error_message) {
			*error_message = "Only Vulkan rendering is supported by the Tico PPSSPP host";
		}
		*ctx = nullptr;
		return false;
	}

	GraphicsContext *graphicsContext = new TicoVulkanGraphicsContext();
	if (!graphicsContext->InitFromRenderThread(error_message)) {
		delete graphicsContext;
		*ctx = nullptr;
		return false;
	}

	gfx_ = graphicsContext;
	*ctx = graphicsContext;
	return true;
}

void TicoGraphicsHost::Shutdown() {
	if (!gfx_) {
		return;
	}

	gfx_->Shutdown();
	delete gfx_;
	gfx_ = nullptr;
}

#endif
