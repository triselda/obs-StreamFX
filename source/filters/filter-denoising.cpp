// Copyright (c) 2020 Michael Fabian Dirks <info@xaymar.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "filter-denoising.hpp"
#include <algorithm>
#include "obs/gs/gs-helper.hpp"
#include "plugin.hpp"
#include "util/util-logging.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<filter::video_denoising> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

#define ST_I18N "Filter.Denoising"
#define ST_KEY_PROVIDER "Provider"
#define ST_I18N_PROVIDER ST_I18N "." ST_KEY_PROVIDER
#define ST_I18N_PROVIDER_NVIDIA_DENOISING ST_I18N_PROVIDER ".NVIDIA.Denoising"

#ifdef ENABLE_FILTER_DENOISING_NVIDIA
#define ST_KEY_NVIDIA_DENOISING "NVIDIA.Denoising"
#define ST_I18N_NVIDIA_DENOISING ST_I18N "." ST_KEY_NVIDIA_DENOISING
#define ST_KEY_NVIDIA_DENOISING_STRENGTH "NVIDIA.Denoising.Strength"
#define ST_I18N_NVIDIA_DENOISING_STRENGTH ST_I18N "." ST_KEY_NVIDIA_DENOISING_STRENGTH
#define ST_I18N_NVIDIA_DENOISING_STRENGTH_WEAK ST_I18N_NVIDIA_DENOISING_STRENGTH ".Weak"
#define ST_I18N_NVIDIA_DENOISING_STRENGTH_STRONG ST_I18N_NVIDIA_DENOISING_STRENGTH ".Strong"
#endif

using streamfx::filter::denoising::denoising_factory;
using streamfx::filter::denoising::denoising_instance;
using streamfx::filter::denoising::denoising_provider;

static constexpr std::string_view HELP_URL = "https://github.com/Xaymar/obs-StreamFX/wiki/Filter-Denoising";

static denoising_provider provider_priority[] = {
	denoising_provider::NVIDIA_DENOISING,
};

const char* streamfx::filter::denoising::cstring(denoising_provider provider)
{
	switch (provider) {
	case denoising_provider::INVALID:
		return "N/A";
	case denoising_provider::AUTOMATIC:
		return D_TRANSLATE(S_STATE_AUTOMATIC);
	case denoising_provider::NVIDIA_DENOISING:
		return D_TRANSLATE(ST_I18N_PROVIDER_NVIDIA_DENOISING);
	default:
		throw std::runtime_error("Missing Conversion Entry");
	}
}

std::string streamfx::filter::denoising::string(denoising_provider provider)
{
	return cstring(provider);
}

//------------------------------------------------------------------------------
// Instance
//------------------------------------------------------------------------------
denoising_instance::denoising_instance(obs_data_t* data, obs_source_t* self)
	: obs::source_instance(data, self),

	  _size(1, 1), _provider_ready(false), _provider(denoising_provider::INVALID), _provider_lock(), _provider_task(),
	  _input(), _output()
{
	{
		::streamfx::obs::gs::context gctx;

		// Create the render target for the input buffering.
		_input = std::make_shared<::streamfx::obs::gs::rendertarget>(GS_RGBA_UNORM, GS_ZS_NONE);
		_input->render(1, 1); // Preallocate the RT on the driver and GPU.
		_output = _input->get_texture();
	}

	if (data) {
		load(data);
	}
}

denoising_instance::~denoising_instance()
{
	// TODO: Make this asynchronous.
	std::unique_lock<std::mutex> ul(_provider_lock);
	switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
	case denoising_provider::NVIDIA_DENOISING:
		nvvfx_denoising_unload();
		break;
#endif
	default:
		break;
	}
}

void denoising_instance::load(obs_data_t* data)
{
	update(data);
}

void denoising_instance::migrate(obs_data_t* data, uint64_t version) {}

void denoising_instance::update(obs_data_t* data)
{
	// Check if the user changed which Denoising provider we use.
	denoising_provider provider = static_cast<denoising_provider>(obs_data_get_int(data, ST_KEY_PROVIDER));
	if (provider == denoising_provider::AUTOMATIC) {
		provider = denoising_factory::get()->find_ideal_provider();
	}

	// Check if the provider was changed, and if so switch.
	if (provider != _provider) {
		_provider_ui = provider;
		switch_provider(provider);
	}

	if (_provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
		case denoising_provider::NVIDIA_DENOISING:
			nvvfx_denoising_update(data);
			break;
#endif
		default:
			break;
		}
	}
}

void streamfx::filter::denoising::denoising_instance::properties(obs_properties_t* properties)
{
	switch (_provider_ui) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
	case denoising_provider::NVIDIA_DENOISING:
		nvvfx_denoising_properties(properties);
		break;
#endif
	default:
		break;
	}
}

uint32_t streamfx::filter::denoising::denoising_instance::get_width()
{
	return std::max<uint32_t>(_size.first, 1);
}

uint32_t streamfx::filter::denoising::denoising_instance::get_height()
{
	return std::max<uint32_t>(_size.second, 1);
}

void denoising_instance::video_tick(float_t time)
{
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	_size       = {width, height};

	// Allow the provider to restrict the size.
	if (target && _provider_ready) {
		std::unique_lock<std::mutex> ul(_provider_lock);

		switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
		case denoising_provider::NVIDIA_DENOISING:
			nvvfx_denoising_size();
			break;
#endif
		default:
			break;
		}
	}

	_dirty = true;
}

void denoising_instance::video_render(gs_effect_t* effect)
{
	auto parent = obs_filter_get_parent(_self);
	auto target = obs_filter_get_target(_self);
	auto width  = obs_source_get_base_width(target);
	auto height = obs_source_get_base_height(target);
	vec4 blank  = vec4{0, 0, 0, 0};

	// Ensure we have the bare minimum of valid information.
	target = target ? target : parent;
	effect = effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);

	// Skip the filter if:
	// - The Provider isn't ready yet.
	// - We don't have a target.
	// - The width/height of the next filter in the chain is empty.
	if (!_provider_ready || !target || (width == 0) || (height == 0)) {
		obs_source_skip_video_filter(_self);
		return;
	}

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler0{::streamfx::obs::gs::debug_color_source, "StreamFX Denoising"};
	::streamfx::obs::gs::debug_marker profiler0_0{::streamfx::obs::gs::debug_color_gray, "'%s' on '%s'",
												  obs_source_get_name(_self), obs_source_get_name(parent)};
#endif

	if (_dirty) { // Lock the provider from being changed.
		std::unique_lock<std::mutex> ul(_provider_lock);

		{ // Allow the provider to restrict the size.
			switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
			case denoising_provider::NVIDIA_DENOISING:
				nvvfx_denoising_size();
				break;
#endif
			default:
				_size = {width, height};
				break;
			}
		}

		// Capture the input.
		if (obs_source_process_filter_begin(_self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
			auto op = _input->render(width, height);

			// Clear the buffer
			gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &blank, 0, 0);

			// Set GPU state
			gs_blend_state_push();
			gs_enable_color(true, true, true, true);
			gs_enable_blending(false);
			gs_enable_depth_test(false);
			gs_enable_stencil_test(false);
			gs_set_cull_mode(GS_NEITHER);

			// Render
			bool srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(gs_get_linear_srgb());
			obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
			gs_enable_framebuffer_srgb(srgb);

			// Reset GPU state
			gs_blend_state_pop();
		} else {
			obs_source_skip_video_filter(_self);
			return;
		}

		// Process the captured input with the provider.
		{
			switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
			case denoising_provider::NVIDIA_DENOISING:
				nvvfx_denoising_process();
				break;
#endif
			default:
				_output = nullptr;
				break;
			}

			if (!_output) {
				D_LOG_ERROR("Provider '%s' did not return a result.", cstring(_provider));
				obs_source_skip_video_filter(_self);
				return;
			}
		}

		// Unlock the provider, as we are no longer doing critical work with it.
	}

	if (_dirty) {
		// Lock the provider from being changed.
		std::unique_lock<std::mutex> ul(_provider_lock);

		{ // Capture the incoming frame.
#ifdef ENABLE_PROFILING
			::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_capture, "Capture"};
#endif
			if (obs_source_process_filter_begin(_self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
				auto op = _input->render(_size.first, _size.second);

				// Matrix
				gs_matrix_push();
				gs_ortho(0., 1., 0., 1., 0., 1.);

				// Clear the buffer
				gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &blank, 0, 0);

				// Set GPU state
				gs_blend_state_push();
				gs_enable_color(true, true, true, true);
				gs_enable_blending(false);
				gs_enable_depth_test(false);
				gs_enable_stencil_test(false);
				gs_set_cull_mode(GS_NEITHER);

				// Render
#ifdef ENABLE_PROFILING
				::streamfx::obs::gs::debug_marker profiler2{::streamfx::obs::gs::debug_color_capture, "Storage"};
#endif
				obs_source_process_filter_end(_self, obs_get_base_effect(OBS_EFFECT_DEFAULT), 1, 1);

				// Reset GPU state
				gs_blend_state_pop();
				gs_matrix_pop();
			} else {
				obs_source_skip_video_filter(_self);
				return;
			}
		}

		try { // Process the captured input with the provider.
#ifdef ENABLE_PROFILING
			::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_convert, "Process"};
#endif
			switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
			case denoising_provider::NVIDIA_DENOISING:
				nvvfx_denoising_process();
				break;
#endif
			default:
				_output.reset();
				break;
			}
		} catch (...) {
			obs_source_skip_video_filter(_self);
			return;
		}

		if (!_output) {
			D_LOG_ERROR("Provider '%s' did not return a result.", cstring(_provider));
			obs_source_skip_video_filter(_self);
			return;
		}

		_dirty = false;
	}

	{ // Draw the result for the next filter to use.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_render, "Render"};
#endif
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), _output->get_object());
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(nullptr, 0, _size.first, _size.second);
		}
	}
}

struct switch_provider_data_t {
	denoising_provider provider;
};

void streamfx::filter::denoising::denoising_instance::switch_provider(denoising_provider provider)
{
	std::unique_lock<std::mutex> ul(_provider_lock);

	// Safeguard against calls made from unlocked memory.
	if (provider == _provider) {
		return;
	}

	// This doesn't work correctly.
	// - Need to allow multiple switches at once because OBS is weird.
	// - Doesn't guarantee that the task is properly killed off.

	// Log information.
	D_LOG_INFO("Instance '%s' is switching provider from '%s' to '%s'.", obs_source_get_name(_self), cstring(_provider),
			   cstring(provider));

	// 1.If there is an existing task, attempt to cancel it.
	if (_provider_task) {
		streamfx::threadpool()->pop(_provider_task);
	}

	// 2. Build data to pass into the task.
	auto spd      = std::make_shared<switch_provider_data_t>();
	spd->provider = _provider;
	_provider     = provider;

	// 3. Then spawn a new task to switch provider.
	_provider_task = streamfx::threadpool()->push(
		std::bind(&denoising_instance::task_switch_provider, this, std::placeholders::_1), spd);
}

void streamfx::filter::denoising::denoising_instance::task_switch_provider(util::threadpool_data_t data)
{
	std::shared_ptr<switch_provider_data_t> spd = std::static_pointer_cast<switch_provider_data_t>(data);

	// 1. Mark the provider as no longer ready.
	_provider_ready = false;

	// 2. Lock the provider from being used.
	std::unique_lock<std::mutex> ul(_provider_lock);

	try {
		// 3. Unload the previous provider.
		switch (spd->provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
		case denoising_provider::NVIDIA_DENOISING:
			nvvfx_denoising_unload();
			break;
#endif
		default:
			break;
		}

		// 4. Load the new provider.
		switch (_provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
		case denoising_provider::NVIDIA_DENOISING:
			nvvfx_denoising_load();
			break;
#endif
		default:
			break;
		}

		// Log information.
		D_LOG_INFO("Instance '%s' switched provider from '%s' to '%s'.", obs_source_get_name(_self),
				   cstring(spd->provider), cstring(_provider));

		_provider_ready = true;
	} catch (std::exception const& ex) {
		// Log information.
		D_LOG_ERROR("Instance '%s' failed switching provider with error: %s", obs_source_get_name(_self), ex.what());
	}
}

#ifdef ENABLE_FILTER_DENOISING_NVIDIA
void streamfx::filter::denoising::denoising_instance::nvvfx_denoising_load()
{
	_nvidia_fx = std::make_shared<::streamfx::nvidia::vfx::denoising>();
}

void streamfx::filter::denoising::denoising_instance::nvvfx_denoising_unload()
{
	_nvidia_fx.reset();
}

void streamfx::filter::denoising::denoising_instance::nvvfx_denoising_size()
{
	if (!_nvidia_fx) {
		return;
	}

	_nvidia_fx->size(_size);
}

void streamfx::filter::denoising::denoising_instance::nvvfx_denoising_process()
{
	if (!_nvidia_fx) {
		_output = _input->get_texture();
		return;
	}

	_output = _nvidia_fx->process(_input->get_texture());
}

void streamfx::filter::denoising::denoising_instance::nvvfx_denoising_properties(obs_properties_t* props)
{
	obs_properties_t* grp = obs_properties_create();
	obs_properties_add_group(props, ST_KEY_NVIDIA_DENOISING, D_TRANSLATE(ST_I18N_NVIDIA_DENOISING), OBS_GROUP_NORMAL,
							 grp);

	{
		auto p = obs_properties_add_list(grp, ST_KEY_NVIDIA_DENOISING_STRENGTH,
										 D_TRANSLATE(ST_I18N_NVIDIA_DENOISING_STRENGTH), OBS_COMBO_TYPE_LIST,
										 OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_NVIDIA_DENOISING_STRENGTH_WEAK), 0);
		obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_NVIDIA_DENOISING_STRENGTH_STRONG), 1);
	}
}

void streamfx::filter::denoising::denoising_instance::nvvfx_denoising_update(obs_data_t* data)
{
	if (!_nvidia_fx)
		return;

	_nvidia_fx->set_strength(
		static_cast<float>(obs_data_get_int(data, ST_KEY_NVIDIA_DENOISING_STRENGTH) == 0 ? 0. : 1.));
}

#endif

//------------------------------------------------------------------------------
// Factory
//------------------------------------------------------------------------------
denoising_factory::~denoising_factory() {}

denoising_factory::denoising_factory()
{
	bool any_available = false;

	// 1. Try and load any configured providers.
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
	try {
		// Load CVImage and Video Effects SDK.
		_nvcuda           = ::streamfx::nvidia::cuda::obs::get();
		_nvcvi            = ::streamfx::nvidia::cv::cv::get();
		_nvvfx            = ::streamfx::nvidia::vfx::vfx::get();
		_nvidia_available = true;
		any_available |= _nvidia_available;
	} catch (const std::exception& ex) {
		_nvidia_available = false;
		_nvvfx.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA providers available due to error: %s", ex.what());
	} catch (...) {
		_nvidia_available = false;
		_nvvfx.reset();
		_nvcvi.reset();
		_nvcuda.reset();
		D_LOG_WARNING("Failed to make NVIDIA providers available with unknown error.", nullptr);
	}
#endif

	// 2. Check if any of them managed to load at all.
	if (!any_available) {
		D_LOG_ERROR("All supported providers failed to initialize, disabling effect.", 0);
		return;
	}

	// 3. In any other case, register the filter!
	_info.id           = S_PREFIX "filter-video-denoising";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO;

	set_resolution_enabled(true);
	finish_setup();
}

const char* denoising_factory::get_name()
{
	return D_TRANSLATE(ST_I18N);
}

void denoising_factory::get_defaults2(obs_data_t* data)
{
	obs_data_set_default_int(data, ST_KEY_PROVIDER, static_cast<int64_t>(denoising_provider::AUTOMATIC));

#ifdef ENABLE_FILTER_DENOISING_NVIDIA
	obs_data_set_default_double(data, ST_KEY_NVIDIA_DENOISING_STRENGTH, 1.);
#endif
}

static bool modified_provider(obs_properties_t* props, obs_property_t*, obs_data_t* settings) noexcept
try {
	return true;
} catch (const std::exception& ex) {
	DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
	return false;
} catch (...) {
	DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
	return false;
}

obs_properties_t* denoising_factory::get_properties2(denoising_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

#ifdef ENABLE_FRONTEND
	{
		obs_properties_add_button2(pr, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN), denoising_factory::on_manual_open,
								   nullptr);
	}
#endif

	if (data) {
		data->properties(pr);
	}

	{ // Advanced Settings
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, S_ADVANCED, D_TRANSLATE(S_ADVANCED), OBS_GROUP_NORMAL, grp);

		{
			auto p = obs_properties_add_list(grp, ST_KEY_PROVIDER, D_TRANSLATE(ST_I18N_PROVIDER), OBS_COMBO_TYPE_LIST,
											 OBS_COMBO_FORMAT_INT);
			obs_property_set_modified_callback(p, modified_provider);
			obs_property_list_add_int(p, D_TRANSLATE(S_STATE_AUTOMATIC),
									  static_cast<int64_t>(denoising_provider::AUTOMATIC));
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_PROVIDER_NVIDIA_DENOISING),
									  static_cast<int64_t>(denoising_provider::NVIDIA_DENOISING));
		}
	}

	return pr;
}

#ifdef ENABLE_FRONTEND
bool denoising_factory::on_manual_open(obs_properties_t* props, obs_property_t* property, void* data)
{
	streamfx::open_url(HELP_URL);
	return false;
}
#endif

bool streamfx::filter::denoising::denoising_factory::is_provider_available(denoising_provider provider)
{
	switch (provider) {
#ifdef ENABLE_FILTER_DENOISING_NVIDIA
	case denoising_provider::NVIDIA_DENOISING:
		return _nvidia_available;
#endif
	default:
		return false;
	}
}

denoising_provider streamfx::filter::denoising::denoising_factory::find_ideal_provider()
{
	for (auto v : provider_priority) {
		if (is_provider_available(v)) {
			return v;
			break;
		}
	}
	return denoising_provider::AUTOMATIC;
}

std::shared_ptr<denoising_factory> _video_denoising_factory_instance = nullptr;

void denoising_factory::initialize()
try {
	if (!_video_denoising_factory_instance)
		_video_denoising_factory_instance = std::make_shared<denoising_factory>();
} catch (const std::exception& ex) {
	D_LOG_ERROR("Failed to initialize due to error: %s", ex.what());
} catch (...) {
	D_LOG_ERROR("Failed to initialize due to unknown error.", "");
}

void denoising_factory::finalize()
{
	_video_denoising_factory_instance.reset();
}

std::shared_ptr<denoising_factory> denoising_factory::get()
{
	return _video_denoising_factory_instance;
}
