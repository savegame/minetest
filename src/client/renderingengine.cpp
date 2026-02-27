// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2017 nerzhul, Loic Blot <loic.blot@unix-experience.fr>

#include <optional>
#include <irrlicht.h>
#include "IMeshCache.h"
#include "fontengine.h"
#include "client.h"
#include "clouds.h"
#include "util/numeric.h"
#include "guiscalingfilter.h"
#include "localplayer.h"
#include "client/hud.h"
#include "client/texturesource.h"
#include "camera.h"
#include "minimap.h"
#include "clientmap.h"
#include "renderingengine.h"
#include "render/core.h"
#include "render/factory.h"
#include "filesys.h"
#include "irrlicht_changes/static_text.h"
#include "irr_ptr.h"

RenderingEngine *RenderingEngine::s_singleton = nullptr;
const video::SColor RenderingEngine::MENU_SKY_COLOR = video::SColor(255, 140, 186, 250);
v2u32 s_screen_size;

/* Helper classes */

class RotationEventReciever : public IEventReceiver {
public:
	RotationEventReciever() {
	}

	virtual bool OnEvent(const SEvent &event) {
		if (event.EventType != EET_USER_EVENT)
			return false;
		if (event.UserEvent.UserData1 != EAET_DISPLAY_ORIENTATION_CHANGED)
			return false;

		if (!RenderingEngine::s_singleton)
			return false;
		
		RenderingEngine::s_singleton->setScreenRotation(static_cast<u16>(event.UserEvent.UserData2));
		return true;
	}
};

void FpsControl::reset()
{
	last_time = porting::getTimeUs();
}

void FpsControl::limit(IrrlichtDevice *device, f32 *dtime)
{
	const float fps_limit = device->isWindowFocused()
			? g_settings->getFloat("fps_max")
			: g_settings->getFloat("fps_max_unfocused");
	const u64 frametime_min = 1000000.0f / std::max(fps_limit, 1.0f);

	u64 time = porting::getTimeUs();

	if (time > last_time) // Make sure time hasn't overflowed
		busy_time = time - last_time;
	else
		busy_time = 0;

	if (busy_time < frametime_min) {
		sleep_time = frametime_min - busy_time;
		porting::preciseSleepUs(sleep_time);
	} else {
		sleep_time = 0;
	}

	// Read the timer again to accurately determine how long we actually slept,
	// rather than calculating it by adding sleep_time to time.
	time = porting::getTimeUs();

	if (time > last_time) // Make sure last_time hasn't overflowed
		*dtime = (time - last_time) / 1000000.0f;
	else
		*dtime = 0;

	last_time = time;
}

class FogShaderUniformSetter : public IShaderUniformSetter
{
	CachedPixelShaderSetting<float, 4> m_fog_color{"fogColor"};
	CachedPixelShaderSetting<float> m_fog_distance{"fogDistance"};
	CachedPixelShaderSetting<float> m_fog_shading_parameter{"fogShadingParameter"};

public:
	void onSetUniforms(video::IMaterialRendererServices *services) override
	{
		auto *driver = services->getVideoDriver();
		assert(driver);

		video::SColor fog_color(0);
		video::E_FOG_TYPE fog_type = video::EFT_FOG_LINEAR;
		f32 fog_start = 0;
		f32 fog_end = 0;
		f32 fog_density = 0;
		bool fog_pixelfog = false;
		bool fog_rangefog = false;
		driver->getFog(fog_color, fog_type, fog_start, fog_end, fog_density,
				fog_pixelfog, fog_rangefog);

		video::SColorf fog_colorf(fog_color);
		m_fog_color.set(fog_colorf, services);

		m_fog_distance.set(&fog_end, services);

		float parameter = 0;
		if (fog_end > 0)
			parameter = 1.0f / (1.0f - fog_start / fog_end);
		m_fog_shading_parameter.set(&parameter, services);
	}
};

IShaderUniformSetter *FogShaderUniformSetterFactory::create()
{
	return new FogShaderUniformSetter();
}

/* Other helpers */

static std::optional<video::E_DRIVER_TYPE> chooseVideoDriver()
{
	auto &&configured_name = g_settings->get("video_driver");
	if (configured_name.empty())
		return std::nullopt;

	auto &&drivers = RenderingEngine::getSupportedVideoDrivers();
	for (auto driver: drivers) {
		auto &&info = RenderingEngine::getVideoDriverInfo(driver);
		if (!strcasecmp(configured_name.c_str(), info.name.c_str()))
			return driver;
	}

	errorstream << "Invalid video_driver specified: " << configured_name << std::endl;
	return std::nullopt;
}

static inline auto getVideoDriverName(video::E_DRIVER_TYPE driver)
{
	return RenderingEngine::getVideoDriverInfo(driver).friendly_name;
}

static IrrlichtDevice *createDevice(SIrrlichtCreationParameters params, std::optional<video::E_DRIVER_TYPE> requested_driver)
{
	if (requested_driver) {
		params.DriverType = *requested_driver;
		infostream << "Trying video driver " << getVideoDriverName(params.DriverType) << std::endl;
		if (auto *device = createDeviceEx(params))
			return device;
		errorstream << "Failed to initialize the " << getVideoDriverName(params.DriverType) << " video driver" << std::endl;
	}
	sanity_check(requested_driver != video::EDT_NULL);

	// try to find any working video driver
	for (auto fallback_driver: RenderingEngine::getSupportedVideoDrivers()) {
		if (fallback_driver == video::EDT_NULL || fallback_driver == requested_driver)
			continue;
		params.DriverType = fallback_driver;
		infostream << "Trying video driver " << getVideoDriverName(params.DriverType) << std::endl;
		if (auto *device = createDeviceEx(params))
			return device;
	}

	throw std::runtime_error("Could not initialize the device with any supported video driver");
}

/* RenderingEngine class */

RenderingEngine::RenderingEngine(MyEventReceiver *receiver)
{
	sanity_check(!s_singleton);

	// Resolution selection
	bool fullscreen = g_settings->getBool("fullscreen");
#ifdef __ANDROID__
	u16 screen_w = 0, screen_h = 0;
	bool window_maximized = false;
#else
	u16 screen_w = std::max<u16>(g_settings->getU16("screen_w"), 1);
	u16 screen_h = std::max<u16>(g_settings->getU16("screen_h"), 1);
	// If I…
	// 1. … set fullscreen = true and window_maximized = true on startup
	// 2. … set fullscreen = false later
	// on Linux with SDL, everything breaks.
	// => Don't do it.
	bool window_maximized = !fullscreen && g_settings->getBool("window_maximized");
#endif

	// bpp, fsaa, vsync
	bool vsync = g_settings->getBool("vsync");
	// Don't enable MSAA in OpenGL context creation if post-processing is enabled,
	// the post-processing pipeline handles it.
	bool enable_fsaa = g_settings->get("antialiasing") == "fsaa" &&
			!g_settings->getBool("enable_post_processing");
	u16 fsaa = enable_fsaa ? MYMAX(2, g_settings->getU16("fsaa")) : 0;

	// Determine driver
	auto driverType = chooseVideoDriver();

	SIrrlichtCreationParameters params = SIrrlichtCreationParameters();
	if (tracestream)
		params.LoggingLevel = ELL_DEBUG;
	params.WindowSize = core::dimension2d<u32>(screen_w, screen_h);
	params.AntiAlias = fsaa;
	params.Fullscreen = fullscreen;
	params.WindowMaximized = window_maximized;
	params.WindowResizable = 1; // 1 means always (required for window_maximized)
	params.Stencilbuffer = false;
	params.Vsync = vsync;
	params.EventReceiver = receiver;
	params.DriverDebug = g_settings->getBool("opengl_debug");

	// there is no standardized path for these on desktop
	std::string rel_path = std::string("client") + DIR_DELIM
			+ "shaders" + DIR_DELIM + "Irrlicht";
	params.OGLES2ShaderPath = (porting::path_share + DIR_DELIM + rel_path + DIR_DELIM).c_str();

	m_device = createDevice(params, driverType);
	driver = m_device->getVideoDriver();
	verbosestream << "Using the " << getVideoDriverName(driver->getDriverType()) << " video driver" << std::endl;

	// This changes the minimum allowed number of vertices in a VBO. Default is 500.setScreenRotation
	driver->setMinHardwareBufferVertexCount(4);

	m_receiver = receiver;

	s_singleton = this;

	g_settings->registerChangedCallback("fullscreen", settingChangedCallback, this);
	g_settings->registerChangedCallback("window_maximized", settingChangedCallback, this);

	// s_screen_size = v2u32{1089,2460};
	s_screen_size =  driver->getScreenSize();
	// m_screen_rotation = 90;
	// setScreenRotation(0);
	setScreenRotation(90);
	m_device->setScreenRotation(90);
#ifdef _AURORAOS_
	m_bufferRotationEventReceiver = new RotationEventReciever();
	m_receiver->setRotationEventReceiver(m_bufferRotationEventReceiver);
	// m_device->setEventReceiver(m_bufferRotationEventReceiver);
#endif
}

RenderingEngine::~RenderingEngine()
{
	sanity_check(s_singleton == this);

	g_settings->deregisterAllChangedCallbacks(this);

	// === Добавить очистку rotation render target ===
	if (m_rotation_rt) {
		driver->removeTexture(m_rotation_rt);
		m_rotation_rt = nullptr;
	}
	if (m_bufferRotationEventReceiver) {
		delete m_bufferRotationEventReceiver;
		m_bufferRotationEventReceiver = nullptr;
	}
	// === Конец добавления ===

	core.reset();
	m_device->closeDevice();
	m_device->drop();
	s_singleton = nullptr;
}

void RenderingEngine::settingChangedCallback(const std::string &name, void *data)
{
	IrrlichtDevice *device = static_cast<RenderingEngine*>(data)->m_device;
	if (name == "fullscreen") {
		device->setFullscreen(g_settings->getBool("fullscreen"));

	} else if (name == "window_maximized") {
		if (!device->isFullscreen()) {
			if (g_settings->getBool("window_maximized"))
				device->maximizeWindow();
			else
				device->restoreWindow();
		}
	}
}

v2u32 RenderingEngine::_getWindowSize() const
{
	if (core)
		return core->getVirtualSize();
#ifdef _AURORAOS_
	v2u32 screen = m_device->getWindowSize(); //real window size
	if (m_screen_rotation == 90 || m_screen_rotation == 270)
		return v2u32(screen.Y, screen.X);  // Swapped
	return screen;
#else
	return m_device->getVideoDriver()->getScreenSize();
#endif
}

void RenderingEngine::setResizable(bool resize)
{
	m_device->setResizable(resize);
}

void RenderingEngine::removeMesh(const scene::IMesh* mesh)
{
	m_device->getSceneManager()->getMeshCache()->removeMesh(mesh);
}

void RenderingEngine::cleanupMeshCache()
{
	auto mesh_cache = m_device->getSceneManager()->getMeshCache();
	mesh_cache->clear();
}

bool RenderingEngine::setupTopLevelWindow()
{
	return setWindowIcon();
}

bool RenderingEngine::setWindowIcon()
{
	irr_ptr<video::IImage> img(driver->createImageFromFile(
			(porting::path_share + "/textures/base/pack/logo.png").c_str()));
	if (!img) {
		warningstream << "Could not load icon file." << std::endl;
		return false;
	}

	return m_device->setWindowIcon(img.get());
}

/*
	Draws a screen with a single text on it.
	Text will be removed when the screen is drawn the next time.
	Additionally, a progressbar can be drawn when percent is set between 0 and 100.
*/
void RenderingEngine::draw_load_screen(const std::wstring &text,
		gui::IGUIEnvironment *guienv, ITextureSource *tsrc, float dtime,
		int percent, float *indef_pos)
{
	v2u32 screensize = getWindowSize();

	v2s32 textsize(g_fontengine->getTextWidth(text), g_fontengine->getLineHeight());
	v2s32 center(screensize.X / 2, screensize.Y / 2);
	core::rect<s32> textrect(center - textsize / 2, center + textsize / 2);

	gui::IGUIStaticText *guitext =
			gui::StaticText::add(guienv, text, textrect, false, false);
	guitext->setTextAlignment(gui::EGUIA_CENTER, gui::EGUIA_UPPERLEFT);

	auto *driver = get_video_driver();

	driver->setFog(RenderingEngine::MENU_SKY_COLOR);
	beginFrame();
	driver->beginScene(true, true, RenderingEngine::MENU_SKY_COLOR);
	if (g_settings->getBool("menu_clouds")) {
		g_menuclouds->step(dtime * 3);
		g_menucloudsmgr->drawAll();
	}

	int percent_min = 0;
	int percent_max = percent;
	if (indef_pos) {
		*indef_pos = fmodf(*indef_pos + (dtime * 50.0f), 140.0f);
		percent_max = std::min((int) *indef_pos, 100);
		percent_min = std::max((int) *indef_pos - 40, 0);
	}
	// draw progress bar
	if ((percent_min >= 0) && (percent_max <= 100)) {
		video::ITexture *progress_img = tsrc->getTexture("progress_bar.png");
		video::ITexture *progress_img_bg =
				tsrc->getTexture("progress_bar_bg.png");

		if (progress_img && progress_img_bg) {
#ifndef __ANDROID__
			const core::dimension2d<u32> &img_size =
					progress_img_bg->getSize();
			float density = g_settings->getFloat("gui_scaling", 0.5f, 20.0f) *
					getDisplayDensity();
			u32 imgW = rangelim(img_size.Width, 200, 600) * density;
			u32 imgH = rangelim(img_size.Height, 24, 72) * density;
#else
			const core::dimension2d<u32> img_size(256, 48);
			float imgRatio = (float)img_size.Height / img_size.Width;
			u32 imgW = screensize.X / 2.2f;
			u32 imgH = floor(imgW * imgRatio);
#endif
			v2s32 img_pos((screensize.X - imgW) / 2,
					(screensize.Y - imgH) / 2);

			draw2DImageFilterScaled(get_video_driver(), progress_img_bg,
					core::rect<s32>(img_pos.X, img_pos.Y,
							img_pos.X + imgW,
							img_pos.Y + imgH),
					core::rect<s32>(0, 0, img_size.Width,
							img_size.Height),
					0, 0, true);

			draw2DImageFilterScaled(get_video_driver(), progress_img,
					core::rect<s32>(img_pos.X + (percent_min * imgW) / 100, img_pos.Y,
							img_pos.X + (percent_max * imgW) / 100,
							img_pos.Y + imgH),
					core::rect<s32>(percent_min * img_size.Width / 100, 0,
							percent_max * img_size.Width / 100,
							img_size.Height),
					0, 0, true);
		}
	}

	guienv->drawAll();
	// driver->endScene();
	endFrame();
	guitext->remove();
}

std::vector<video::E_DRIVER_TYPE> RenderingEngine::getSupportedVideoDrivers()
{
	// Only check these drivers. We do not support software and D3D in any capacity.
	// ordered by preference (best first)
	static const video::E_DRIVER_TYPE glDrivers[] = {
		video::EDT_OPENGL,
		video::EDT_OPENGL3,
		video::EDT_OGLES2,
		video::EDT_NULL,
	};
	std::vector<video::E_DRIVER_TYPE> drivers;

	for (auto driver : glDrivers) {
		if (IrrlichtDevice::isDriverSupported(driver))
			drivers.push_back(driver);
	}

	return drivers;
}

void RenderingEngine::initialize(Client *client, Hud *hud)
{
	const std::string &draw_mode = g_settings->get("3d_mode");
	core.reset(createRenderingCore(draw_mode, m_device, client, hud));
}

void RenderingEngine::finalize()
{
	core.reset();
}

void RenderingEngine::draw_scene(video::SColor skycolor, bool show_hud,
		bool draw_wield_tool, bool draw_crosshair)
{
	core->draw(skycolor, show_hud, draw_wield_tool, draw_crosshair);
}

const VideoDriverInfo &RenderingEngine::getVideoDriverInfo(video::E_DRIVER_TYPE type)
{
	static const std::unordered_map<int, VideoDriverInfo> driver_info_map = {
		{(int)video::EDT_NULL,   {"null",   "NULL Driver"}},
		{(int)video::EDT_OPENGL, {"opengl", "OpenGL"}},
		{(int)video::EDT_OPENGL3, {"opengl3", "OpenGL 3+"}},
		{(int)video::EDT_OGLES2, {"ogles2", "OpenGL ES2"}},
	};
	return driver_info_map.at((int)type);
}

float RenderingEngine::getDisplayDensity()
{
	float user_factor = g_settings->getFloat("display_density_factor", 0.5f, 5.0f);
#ifndef __ANDROID__
	float dpi = get_raw_device()->getDisplayDensity();
	if (dpi == 0.0f)
		dpi = 96.0f;
	return std::max(dpi / 96.0f * user_factor, 0.5f);
#else // __ANDROID__
	return porting::getDisplayDensity() * user_factor;
#endif // __ANDROID__
}

void RenderingEngine::autosaveScreensizeAndCo(
		const core::dimension2d<u32> initial_screen_size,
		const bool initial_window_maximized)
{
	if (!g_settings->getBool("autosave_screensize"))
		return;

	// Note: If the screensize or similar hasn't changed (i.e. it's the same as
	// the setting was when minetest started, as given by the initial_* parameters),
	// we do not want to save the thing. This allows users to also manually change
	// the settings.

	// Don't save the fullscreen size, we want the windowed size.
	bool fullscreen = RenderingEngine::get_raw_device()->isFullscreen();
	// Screen size
	const core::dimension2d<u32> current_screen_size = s_screen_size;
		// RenderingEngine::get_video_driver()->getScreenSize();
		// RenderingEngine::getVirtualScreenSize();
	// Don't replace good value with (0, 0)
	if (!fullscreen &&
			current_screen_size != core::dimension2d<u32>(0, 0) &&
			current_screen_size != initial_screen_size) {
		g_settings->setU16("screen_w", current_screen_size.Width);
		g_settings->setU16("screen_h", current_screen_size.Height);
	}

	// Window maximized
	const bool is_window_maximized = RenderingEngine::get_raw_device()
			->isWindowMaximized();
	if (is_window_maximized != initial_window_maximized)
		g_settings->setBool("window_maximized", is_window_maximized);
}

void RenderingEngine::setScreenRotation(u16 degrees)
{
	fprintf(stderr, "Set screen rotation: %i\n", degrees);
	// Validate input
	if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
		warningstream << "Invalid screen rotation: " << degrees 
						<< ", must be 0, 90, 180, or 270" << std::endl;
		return;
	}

	if (m_screen_rotation != degrees) {
		m_screen_rotation = degrees;
		
		// Force recreation of render target on next frame
		if (m_rotation_rt) {
			driver->removeTexture(m_rotation_rt);
			m_rotation_rt = nullptr;
		}
		
		infostream << "Screen rotation set to " << degrees << " degrees" << std::endl;
	}
}

v2u32 RenderingEngine::_getVirtualScreenSize() const
{
	if (m_screen_rotation == 0) {
		return _getWindowSize();
	}

	// If rotation is active, return the framebuffer size
	if (m_framebuffer_size.X > 0 && m_framebuffer_size.Y > 0) {
		// fprintf(stderr, "Get frame buffer size\n");
		return m_framebuffer_size;
	}

	return _getWindowSize();
}

void RenderingEngine::updateRotationRenderTarget()
{
    v2u32 screen = s_screen_size;
	// fprintf(stderr, "Get Screen size from device : %i x %i\n", screen.X, screen.Y);
    v2u32 needed_size;
    
    // For 90° and 270° rotation, swap width and height
    if (m_screen_rotation == 90 || m_screen_rotation == 270) {
        needed_size = v2u32(screen.Y, screen.X);
    } else {
        needed_size = screen;
    }
    
    // Check if we need to recreate the render target
    bool need_recreate = !m_rotation_rt;
    if (m_rotation_rt) {
        core::dimension2du current_size = m_rotation_rt->getSize();
        if (current_size.Width != needed_size.X || current_size.Height != needed_size.Y) {
            need_recreate = true;
        }
    }
    
    if (need_recreate) {
        // Remove old texture if exists
        if (m_rotation_rt) {
            driver->removeTexture(m_rotation_rt);
            m_rotation_rt = nullptr;
        }
        
        // Create new render target texture
        m_rotation_rt = driver->addRenderTargetTexture(
            core::dimension2du(needed_size.X, needed_size.Y),
            "screen_rotation_rt",
            video::ECF_A8R8G8B8
        );
        
        if (!m_rotation_rt) {
            errorstream << "Failed to create rotation render target texture" << std::endl;
            m_screen_rotation = 0;  // Fallback to no rotation
            return;
        }
        
        infostream << "Created rotation render target: " 
                   << needed_size.X << "x" << needed_size.Y << std::endl;
    }
    
    m_framebuffer_size = needed_size;
}

void RenderingEngine::beginFrame()
{
    m_rotation_active = false;
    
    if (m_screen_rotation == 0) {
        return;  // No rotation - nothing to do
    }
    
    updateRotationRenderTarget();
    
    if (!m_rotation_rt) {
        return;  // Failed to create render target
    }
    
    // Set the framebuffer as render target
    driver->setRenderTarget(m_rotation_rt, true, true, video::SColor(0, 0, 0, 0));
    
    // Notify Irrlicht about the new viewport size
    driver->OnResize(core::dimension2du(m_framebuffer_size.X, m_framebuffer_size.Y));
    
    m_rotation_active = true;
}

void RenderingEngine::endFrame()
{
    if (!m_rotation_active) {
        // No rotation was active - just end the scene normally
        driver->endScene();
        return;
    }
    
    // Switch back to screen (default framebuffer)
	v2u32 screen_size = m_device->getWindowSize(); // real SDL window size
    driver->setRenderTarget(nullptr, true, true, video::SColor(255, 0, 0, 0));
    driver->OnResize(core::dimension2du(screen_size.X, screen_size.Y));
    
    // Draw the rotated framebuffer to screen
    drawRotatedFramebuffer();
    
    driver->endScene();
    m_rotation_active = false;
}

void RenderingEngine::drawRotatedFramebuffer()
{
    if (!m_rotation_rt) {
        return;
    }
    
    v2u32 screen_size = driver->getScreenSize();
    core::dimension2du tex_size = m_rotation_rt->getSize();

    // Define the 4 corners of the texture (UV coordinates)
    // Order: top-left, top-right, bottom-right, bottom-left
    core::vector2df uv[4];
    
    switch (m_screen_rotation) {
        case 0:   // No rotation (shouldn't happen, but handle it)
            uv[0] = core::vector2df(0, 0);  // TL
            uv[1] = core::vector2df(1, 0);  // TR
            uv[2] = core::vector2df(1, 1);  // BR
            uv[3] = core::vector2df(0, 1);  // BL
            break;
            
        case 90:  // Rotate 90° clockwise
            // Original top-left goes to top-right, etc.
            uv[0] = core::vector2df(0, 1);  // TL <- was BL
            uv[1] = core::vector2df(0, 0);  // TR <- was TL
            uv[2] = core::vector2df(1, 0);  // BR <- was TR
            uv[3] = core::vector2df(1, 1);  // BL <- was BR
            break;
            
        case 180: // Rotate 180°
            uv[0] = core::vector2df(1, 1);  // TL <- was BR
            uv[1] = core::vector2df(0, 1);  // TR <- was BL
            uv[2] = core::vector2df(0, 0);  // BR <- was TL
            uv[3] = core::vector2df(1, 0);  // BL <- was TR
            break;
            
        case 270: // Rotate 270° clockwise (= 90° counter-clockwise)
            uv[0] = core::vector2df(1, 0);  // TL <- was TR
            uv[1] = core::vector2df(1, 1);  // TR <- was BR
            uv[2] = core::vector2df(0, 1);  // BR <- was BL
            uv[3] = core::vector2df(0, 0);  // BL <- was TL
            break;
            
        default:
            return;
    }
    
    // Screen rectangle (full screen quad)
    // s32 sw = screen_size.X;
    // s32 sh = screen_size.Y;
    
    // Create vertices for the quad
    video::S3DVertex vertices[4];
    
    video::SColor color(255, 255, 255, 255);
    
    // Top-left vertex
    vertices[0] = video::S3DVertex(
        -1.0f, 1.0f, 0.0f,   // position (NDC)
        0, 0, 1,              // normal
        color,
        uv[0].X, uv[0].Y      // texcoord
    );
    
    // Top-right vertex
    vertices[1] = video::S3DVertex(
        1.0f, 1.0f, 0.0f,
        0, 0, 1,
        color,
        uv[1].X, uv[1].Y
    );
    
    // Bottom-right vertex
    vertices[2] = video::S3DVertex(
        1.0f, -1.0f, 0.0f,
        0, 0, 1,
        color,
        uv[2].X, uv[2].Y
    );
    
    // Bottom-left vertex
    vertices[3] = video::S3DVertex(
        -1.0f, -1.0f, 0.0f,
        0, 0, 1,
        color,
        uv[3].X, uv[3].Y
    );
    
    // Triangle indices (two triangles for a quad)
    u16 indices[6] = {0, 1, 2, 0, 2, 3};
    
    // Set up material for drawing
    video::SMaterial material;
    // material.Lighting = false;
    material.ZWriteEnable = video::EZW_OFF;
    material.ZBuffer = video::ECFN_DISABLED;
    material.TextureLayers[0].Texture = m_rotation_rt;
    // material.TextureLayers[0].BilinearFilter = true;
    material.TextureLayers[0].TextureWrapU = video::ETC_CLAMP_TO_EDGE;
    material.TextureLayers[0].TextureWrapV = video::ETC_CLAMP_TO_EDGE;
    
    // Use 2D drawing approach instead (simpler and more reliable)
    // Reset any transforms
    driver->setMaterial(material);
    
    core::matrix4 oldProj = driver->getTransform(video::ETS_PROJECTION);
    core::matrix4 oldView = driver->getTransform(video::ETS_VIEW);
    core::matrix4 oldWorld = driver->getTransform(video::ETS_WORLD);
    
    // Set orthographic projection for 2D drawing
    core::matrix4 proj;
    proj.buildProjectionMatrixOrthoLH(2.0f, 2.0f, -1.0f, 1.0f);
    driver->setTransform(video::ETS_PROJECTION, proj);
    driver->setTransform(video::ETS_VIEW, core::matrix4());
    driver->setTransform(video::ETS_WORLD, core::matrix4());
    
    // Draw the quad
    driver->drawIndexedTriangleList(vertices, 4, indices, 2);
    
    // Restore transforms
    driver->setTransform(video::ETS_PROJECTION, oldProj);
    driver->setTransform(video::ETS_VIEW, oldView);
    driver->setTransform(video::ETS_WORLD, oldWorld);
}