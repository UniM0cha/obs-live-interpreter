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

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define UP_RATE 16000     /* 서버/Gemini 입력 레이트 */
#define CHUNK_BYTES 3200  /* 100ms @ 16kHz s16 mono */

struct interpreter_filter {
	obs_source_t *context = nullptr;

	/* 설정 */
	std::string server_url;   /* 예: ws://localhost:8000 (베이스) */
	std::string service_key;
	std::atomic<bool> enabled{false};

	/* 48k float planar(Nch) -> 16k s16 mono 리샘플러 */
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
};

static void worker_fn(interpreter_filter *f)
{
	std::vector<uint8_t> chunk;
	chunk.reserve(CHUNK_BYTES);
	while (f->running.load()) {
		std::unique_lock<std::mutex> lk(f->buf_mtx);
		f->buf_cv.wait(lk, [f] { return !f->running.load() || f->pcm_buf.size() >= CHUNK_BYTES; });
		while (f->pcm_buf.size() >= CHUNK_BYTES) {
			chunk.assign(f->pcm_buf.begin(), f->pcm_buf.begin() + CHUNK_BYTES);
			f->pcm_buf.erase(f->pcm_buf.begin(), f->pcm_buf.begin() + CHUNK_BYTES);
			lk.unlock();
			if (f->enabled.load() && f->ws_ready.load())
				f->ws.sendBinary(std::string(reinterpret_cast<const char *>(chunk.data()), chunk.size()));
			lk.lock();
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
	bool want = obs_data_get_bool(settings, "enabled");

	std::string url = f->server_url + "/ingress?key=" + f->service_key;
	f->ws.setUrl(url);
	f->ws.stop();
	f->ws_ready = false;
	f->enabled = want;
	if (want) {
		f->ws.start(); /* 자동 재접속 포함 */
		obs_log(LOG_INFO, "[interpreter] 업링크 ON → %s/ingress", f->server_url.c_str());
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
	obs_log(LOG_INFO, "[interpreter] 필터 생성됨");
	return f;
}

static void interpreter_filter_destroy(void *data)
{
	auto *f = static_cast<interpreter_filter *>(data);
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
	obs_data_set_default_bool(settings, "enabled", false);
}

static obs_properties_t *interpreter_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "server_url", obs_module_text("ServerUrl"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(p, "service_key", obs_module_text("ServiceKey"), OBS_TEXT_PASSWORD);
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
				struct resample_info src = {oai.samples_per_sec, AUDIO_FORMAT_FLOAT_PLANAR,
							    oai.speakers};
				struct resample_info dst = {UP_RATE, AUDIO_FORMAT_16BIT, SPEAKERS_MONO};
				f->resampler = audio_resampler_create(&dst, &src);
				obs_log(LOG_INFO, "[interpreter] 리샘플러 생성 %u→%u Hz", oai.samples_per_sec, UP_RATE);
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
					if (f->pcm_buf.size() >= CHUNK_BYTES)
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

void obs_module_unload(void)
{
	ix::uninitNetSystem();
	obs_log(LOG_INFO, "OBS Live Interpreter 언로드됨");
}
