// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2017 nerzhul, Loic Blot <loic.blot@unix-experience.fr>

#pragma once

#include <vector>
#include <memory>
#include <string>
#include "client/inputhandler.h"
#include "debug.h"
#include "config.h"
#include "client/shader.h"
#include "client/render/core.h"
// include the shadow mapper classes too
#include "client/shadows/dynamicshadowsrender.h"
#include <IVideoDriver.h>
#include <ITexture.h>

#if !IS_CLIENT_BUILD
#error Do not include in server builds
#endif

struct VideoDriverInfo {
	std::string name;
	std::string friendly_name;
};

class ITextureSource;
class Client;
class Hud;

class RenderingCore;

// Instead of a mechanism to disable fog we just set it to be really far away
#define FOG_RANGE_ALL (100000 * BS)

/* Helpers */

struct FpsControl {
	FpsControl() : last_time(0), busy_time(0), sleep_time(0) {}

	void reset();

	void limit(IrrlichtDevice *device, f32 *dtime);

	u32 getBusyMs() const { return busy_time / 1000; }

	// all values in microseconds (us)
	u64 last_time, busy_time, sleep_time;
};

// Populates fogColor, fogDistance, fogShadingParameter with values from Irrlicht
class FogShaderUniformSetterFactory : public IShaderUniformSetterFactory
{
public:
	FogShaderUniformSetterFactory() {};
	virtual IShaderUniformSetter *create();
};

#ifdef _AURORAOS_
class RotationEventReciever;
#endif

/* Rendering engine class */
class RenderingEngine
{
#ifdef _AURORAOS_
	friend class RotationEventReciever;
#endif
public:
	static const video::SColor MENU_SKY_COLOR;

	RenderingEngine(MyEventReceiver *eventReceiver);
	~RenderingEngine();

	void setResizable(bool resize);

	video::IVideoDriver *getVideoDriver() { return driver; }

	static const VideoDriverInfo &getVideoDriverInfo(video::E_DRIVER_TYPE type);
	static float getDisplayDensity();

	bool setupTopLevelWindow();
	bool setWindowIcon();
	void cleanupMeshCache();

	void removeMesh(const scene::IMesh* mesh);

	/**
	 * This takes 3d_mode into account - side-by-side will return a
	 * halved horizontal size.
	 *
	 * @return "window" size
	 */
	static v2u32 getWindowSize()
	{
		sanity_check(s_singleton);
		return s_singleton->_getWindowSize();
	}

	io::IFileSystem *get_filesystem()
	{
		return m_device->getFileSystem();
	}

	static video::IVideoDriver *get_video_driver()
	{
		sanity_check(s_singleton && s_singleton->m_device);
		return s_singleton->m_device->getVideoDriver();
	}

	scene::ISceneManager *get_scene_manager()
	{
		return m_device->getSceneManager();
	}

	static IrrlichtDevice *get_raw_device()
	{
		sanity_check(s_singleton && s_singleton->m_device);
		return s_singleton->m_device;
	}

	gui::IGUIEnvironment *get_gui_env()
	{
		return m_device->getGUIEnvironment();
	}

	// If "indef_pos" is given, the value of "percent" is ignored and an indefinite
	// progress bar is drawn.
	void draw_load_screen(const std::wstring &text,
			gui::IGUIEnvironment *guienv, ITextureSource *tsrc,
			float dtime = 0, int percent = 0, float *indef_pos = nullptr);

	void draw_scene(video::SColor skycolor, bool show_hud,
			bool draw_wield_tool, bool draw_crosshair);

	void initialize(Client *client, Hud *hud);
	void finalize();

	bool run()
	{
		return m_device->run();
	}

	// FIXME: this is still global when it shouldn't be
	static ShadowRenderer *get_shadow_renderer()
	{
		if (s_singleton && s_singleton->core)
			return s_singleton->core->get_shadow_renderer();
		return nullptr;
	}
	static std::vector<video::E_DRIVER_TYPE> getSupportedVideoDrivers();

	static void autosaveScreensizeAndCo(
			const core::dimension2d<u32> initial_screen_size,
			const bool initial_window_maximized);

	static PointerType getLastPointerType()
	{
		sanity_check(s_singleton && s_singleton->m_receiver);
		return s_singleton->m_receiver->getLastPointerType();
	}

	// === Screen Rotation API ===
#ifdef _AURORAOS_
	/**
		* Set screen rotation angle.
		* @param degrees Rotation angle: 0, 90, 180, or 270
		*/
	void setScreenRotation(u16 degrees);

	/**
		* Get current screen rotation angle.
		*/
	static u16 getScreenRotation()
	{
		if (s_singleton) {
			return s_singleton->m_screen_rotation;
		}
		return 0;
	}

	/**
		* Begin rendering frame. Call before driver->beginScene().
		* Sets up render target for rotation if needed.
		*/
	void beginFrame();

	/**
		* End rendering frame. Call instead of driver->endScene().
		* Draws rotated framebuffer to screen and calls endScene().
		*/
	void endFrame();

	/**
	* Get the rotation render target texture (or nullptr if rotation not active)
	*/
	static video::ITexture* getRotationRenderTarget()
	{
		if (s_singleton && s_singleton->m_rotation_active) {
			return s_singleton->m_rotation_rt;
		}
		return nullptr;
	}
#endif

	/**
	* Get virtual screen size (with rotation applied).
	* Use this for all rendering calculations.
	*/
	static v2u32 getVirtualScreenSize()
	{
		sanity_check(s_singleton);
		return s_singleton->_getVirtualScreenSize();
	}

private:
	v2u32 _getVirtualScreenSize() const;
	static void settingChangedCallback(const std::string &name, void *data);
	v2u32 _getWindowSize() const;

	std::unique_ptr<RenderingCore> core;
	IrrlichtDevice *m_device = nullptr;
	video::IVideoDriver *driver;
	MyEventReceiver *m_receiver = nullptr;
	static RenderingEngine *s_singleton;

#ifdef _AURORAOS_
	// === Rotation Framebuffer ===
	video::ITexture* m_rotation_rt = nullptr;      // Render target texture
	u16 m_screen_rotation = 0;                      // 0, 90, 180, 270 degrees
	v2u32 m_framebuffer_size{0, 0};                // Size of the framebuffer
	bool m_rotation_active = false;                 // Is rotation currently active for this frame
	RotationEventReciever *m_bufferRotationEventReceiver = nullptr;

	void updateRotationRenderTarget();
	void drawRotatedFramebuffer();
#endif
};
