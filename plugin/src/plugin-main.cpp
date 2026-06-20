/*
OBS Live Interpreter — tap + 리샘플 + WSS 업링크 (M1.1)
Copyright (C) 2026 Jeongyun Lee
GPL-2.0-or-later (libobs 링크)

filter_audio 는 입력 PCM 을 변형 없이 그대로 반환(방송 원본 불변, 불변규칙 #1).
복사본만: 48kHz float planar(Nch) → 16kHz s16 mono 리샘플(libobs audio_resampler) →
워커 스레드에서 IXWebSocket 으로 서버 /ingress 에 업로드. "번역 ON" 토글 시에만 송출.
오디오 콜백은 리샘플+버퍼 append 까지만(네트워크는 워커) — 콜백 비블로킹(불변규칙 #3).
*/
#include <obs-module.h>
#include <plugin-support.h>
#include <media-io/audio-resampler.h>
#include "interpreter-control.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include <obs-frontend-api.h>
#include "interpreter-dock.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* 입력 레이트는 선택 엔진에 따라 동적: Gemini 16k / OpenAI 24k.
 * 100ms 청크(s16 mono) = rate/1000 * 100 * 2 = rate/5 바이트. 기본값은 struct 초기화 참고. */

struct interpreter_filter {
	obs_source_t *context = nullptr;

	/* 설정 */
	std::string server_url;   /* 예: ws://localhost:8000 (베이스) */
	std::string service_key;
	std::string engine;       /* "gemini" | "openai" */
	std::atomic<bool> enabled{false};

	/* 입력 레이트/청크는 엔진에 따라 동적(Gemini 16k·3200B / OpenAI 24k·4800B) */
	std::atomic<uint32_t> up_rate{16000};
	std::atomic<size_t> chunk_bytes{3200};

	/* 48k float planar(Nch) -> up_rate s16 mono 리샘플러 */
	audio_resampler_t *resampler = nullptr;
	std::mutex resampler_mtx;

	/* 업링크 버퍼 + 워커 */
	std::mutex buf_mtx;
	std::condition_variable buf_cv;
	std::vector<uint8_t> pcm_buf;
	std::thread worker;
	std::atomic<bool> running{false};

	/* WebSocket */
	ix::WebSocket ws;
	std::atomic<bool> ws_ready{false};

	/* 외부 토글 핫키 */
	obs_hotkey_id toggle_hotkey = OBS_INVALID_HOTKEY_ID;
};

/* ── 외부 컨트롤(도크/핫키)용 전역 레지스트리 ── */
static std::mutex g_filters_mtx;
static std::vector<interpreter_filter *> g_filters;

/* 필터의 enabled 설정을 바꾸고 표준 경로(update)로 적용 → 체크박스/연결/영속 동기화 */
static void set_filter_enabled(interpreter_filter *f, bool on)
{
	obs_data_t *s = obs_source_get_settings(f->context);
	obs_data_set_bool(s, "enabled", on);
	obs_source_update(f->context, s);
	obs_data_release(s);
}

int interpreter_state(void)
{
	std::lock_guard<std::mutex> lk(g_filters_mtx);
	if (g_filters.empty())
		return INTERP_NONE;
	bool any_enabled = false, any_live = false;
	for (auto *f : g_filters) {
		if (f->enabled.load()) {
			any_enabled = true;
			if (f->ws_ready.load())
				any_live = true;
		}
	}
	return any_live ? INTERP_LIVE : (any_enabled ? INTERP_CONNECTING : INTERP_OFF);
}

void interpreter_toggle_all(void)
{
	std::lock_guard<std::mutex> lk(g_filters_mtx);
	bool any_enabled = false;
	for (auto *f : g_filters)
		any_enabled = any_enabled || f->enabled.load();
	bool target = !any_enabled; /* 하나라도 ON이면 전부 OFF, 아니면 전부 ON */
	for (auto *f : g_filters)
		set_filter_enabled(f, target);
}

static void interpreter_toggle_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	auto *f = static_cast<interpreter_filter *>(data);
	set_filter_enabled(f, !f->enabled.load());
}

static void worker_fn(interpreter_filter *f)
{
	std::vector<uint8_t> chunk;
	while (f->running.load()) {
		std::unique_lock<std::mutex> lk(f->buf_mtx);
		f->buf_cv.wait(lk, [f] { return !f->running.load() || f->pcm_buf.size() >= f->chunk_bytes.load(); });
		size_t cb = f->chunk_bytes.load();
		while (f->pcm_buf.size() >= cb) {
			chunk.assign(f->pcm_buf.begin(), f->pcm_buf.begin() + cb);
			f->pcm_buf.erase(f->pcm_buf.begin(), f->pcm_buf.begin() + cb);
			lk.unlock();
			if (f->enabled.load() && f->ws_ready.load())
				f->ws.sendBinary(std::string(reinterpret_cast<const char *>(chunk.data()), chunk.size()));
			lk.lock();
			cb = f->chunk_bytes.load();
		}
	}
}

static const char *interpreter_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("InterpreterFilter");
}

static void interpreter_filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<interpreter_filter *>(data);
	f->server_url = obs_data_get_string(settings, "server_url");
	f->service_key = obs_data_get_string(settings, "service_key");
	std::string engine = obs_data_get_string(settings, "engine");
	if (engine != "openai")
		engine = "gemini"; /* 화이트리스트(기본 gemini) */
	bool want = obs_data_get_bool(settings, "enabled");

	/* 엔진별 입력 레이트: Gemini 16k / OpenAI 24k. 100ms 청크 = rate/5 바이트(s16 mono). */
	uint32_t new_rate = (engine == "openai") ? 24000u : 16000u;
	if (new_rate != f->up_rate.load()) {
		f->up_rate = new_rate;
		f->chunk_bytes = new_rate / 5;
		/* 레이트 변경 → 리샘플러 재생성(콜백이 새 레이트로 lazy 생성) + 이전 레이트 잔여 폐기 */
		{
			std::lock_guard<std::mutex> rlk(f->resampler_mtx);
			if (f->resampler) {
				audio_resampler_destroy(f->resampler);
				f->resampler = nullptr;
			}
		}
		{
			std::lock_guard<std::mutex> blk(f->buf_mtx);
			f->pcm_buf.clear();
		}
	}
	f->engine = engine;

	std::string url = f->server_url + "/ingress?key=" + f->service_key + "&engine=" + engine;
	f->ws.setUrl(url);
	f->ws.stop();
	f->ws_ready = false;
	f->enabled = want;
	if (want) {
		f->ws.start(); /* 자동 재접속 포함 */
		obs_log(LOG_INFO, "[interpreter] 업링크 ON → %s/ingress (engine=%s, %uHz)", f->server_url.c_str(),
			engine.c_str(), new_rate);
	} else {
		obs_log(LOG_INFO, "[interpreter] 업링크 OFF");
	}
}

static void *interpreter_filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new interpreter_filter();
	f->context = source;
	f->running = true;
	f->worker = std::thread(worker_fn, f);

	f->ws.setOnMessageCallback([f](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open:
			f->ws_ready = true;
			obs_log(LOG_INFO, "[interpreter] 서버 연결됨");
			break;
		case ix::WebSocketMessageType::Close:
			f->ws_ready = false;
			obs_log(LOG_INFO, "[interpreter] 서버 연결 끊김");
			break;
		case ix::WebSocketMessageType::Error:
			f->ws_ready = false;
			obs_log(LOG_WARNING, "[interpreter] WS 오류: %s", msg->errorInfo.reason.c_str());
			break;
		default:
			break;
		}
	});

	interpreter_filter_update(f, settings);

	f->toggle_hotkey = obs_hotkey_register_source(source, "obs_live_interpreter.toggle",
						      obs_module_text("ToggleHotkey"), interpreter_toggle_hotkey, f);
	{
		std::lock_guard<std::mutex> lk(g_filters_mtx);
		g_filters.push_back(f);
	}

	obs_log(LOG_INFO, "[interpreter] 필터 생성됨");
	return f;
}

static void interpreter_filter_destroy(void *data)
{
	auto *f = static_cast<interpreter_filter *>(data);
	{
		std::lock_guard<std::mutex> lk(g_filters_mtx);
		g_filters.erase(std::remove(g_filters.begin(), g_filters.end(), f), g_filters.end());
	}
	obs_hotkey_unregister(f->toggle_hotkey);
	f->enabled = false;
	f->ws.stop();
	f->running = false;
	f->buf_cv.notify_all();
	if (f->worker.joinable())
		f->worker.join();
	{
		std::lock_guard<std::mutex> lk(f->resampler_mtx);
		if (f->resampler)
			audio_resampler_destroy(f->resampler);
	}
	obs_log(LOG_INFO, "[interpreter] 필터 제거");
	delete f;
}

static void interpreter_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server_url", "ws://localhost:8000");
	obs_data_set_default_string(settings, "engine", "gemini");
	obs_data_set_default_bool(settings, "enabled", false);
}

static obs_properties_t *interpreter_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "server_url", obs_module_text("ServerUrl"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(p, "service_key", obs_module_text("ServiceKey"), OBS_TEXT_PASSWORD);
	obs_property_t *eng = obs_properties_add_list(p, "engine", obs_module_text("Engine"), OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(eng, obs_module_text("EngineGemini"), "gemini");
	obs_property_list_add_string(eng, obs_module_text("EngineOpenAI"), "openai");
	obs_properties_add_bool(p, "enabled", obs_module_text("Enabled"));
	return p;
}

/*
 * tap: 입력 그대로 반환(원본 불변). 복사본은 리샘플 후 워커로.
 * 콜백에서 하는 일: 리샘플(빠른 CPU) + 버퍼 append 까지. 네트워크 전송은 워커.
 */
static struct obs_audio_data *interpreter_filter_audio(void *data, struct obs_audio_data *audio)
{
	auto *f = static_cast<interpreter_filter *>(data);
	if (f->enabled.load() && f->ws_ready.load() && audio->frames > 0) {
		std::lock_guard<std::mutex> rlk(f->resampler_mtx);
		if (!f->resampler) {
			struct obs_audio_info oai;
			if (obs_get_audio_info(&oai)) {
				uint32_t rate = f->up_rate.load();
				struct resample_info src = {oai.samples_per_sec, AUDIO_FORMAT_FLOAT_PLANAR,
							    oai.speakers};
				struct resample_info dst = {rate, AUDIO_FORMAT_16BIT, SPEAKERS_MONO};
				f->resampler = audio_resampler_create(&dst, &src);
				obs_log(LOG_INFO, "[interpreter] 리샘플러 생성 %u→%u Hz", oai.samples_per_sec, rate);
			}
		}
		if (f->resampler) {
			uint8_t *out[MAX_AV_PLANES] = {0};
			uint32_t out_frames = 0;
			uint64_t ts_off = 0;
			if (audio_resampler_resample(f->resampler, out, &out_frames, &ts_off,
						     (const uint8_t *const *)audio->data, audio->frames)) {
				size_t bytes = static_cast<size_t>(out_frames) * 2; /* s16 mono */
				if (bytes && out[0]) {
					std::lock_guard<std::mutex> blk(f->buf_mtx);
					f->pcm_buf.insert(f->pcm_buf.end(), out[0], out[0] + bytes);
					if (f->pcm_buf.size() >= f->chunk_bytes.load())
						f->buf_cv.notify_one();
				}
			}
		}
	}
	return audio; /* 원본 그대로 통과 */
}

static struct obs_source_info interpreter_filter_info = {
	.id = "obs_live_interpreter_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = interpreter_filter_get_name,
	.create = interpreter_filter_create,
	.destroy = interpreter_filter_destroy,
	.get_defaults = interpreter_filter_defaults,
	.get_properties = interpreter_filter_properties,
	.update = interpreter_filter_update,
	.filter_audio = interpreter_filter_audio,
};

bool obs_module_load(void)
{
	ix::initNetSystem();
	obs_register_source(&interpreter_filter_info);
	obs_log(LOG_INFO, "OBS Live Interpreter 로드됨 (버전 %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	/* 프론트엔드 준비 후 도크 등록 (보기→도크 메뉴에 노출) */
	auto *dock = new InterpreterDock();
	obs_frontend_add_dock_by_id("obs_live_interpreter_dock", obs_module_text("DockTitle"), dock);
	obs_log(LOG_INFO, "[interpreter] 도크 등록됨");
}

void obs_module_unload(void)
{
	ix::uninitNetSystem();
	obs_log(LOG_INFO, "OBS Live Interpreter 언로드됨");
}
