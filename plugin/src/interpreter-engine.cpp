/*
Glossa — 통역 엔진 구현. 설계는 interpreter-engine.hpp 참고.
*/
#include "interpreter-engine.hpp"
#include "interpreter-control.hpp"
#include <plugin-support.h>

#include <obs-module.h>
#include <util/platform.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

static const uint64_t NS = 1000000000ULL;
static const uint64_t MIX_WINDOW_NS = 100000000ULL; /* 100ms 지연 윈도우 */

InterpreterEngine &InterpreterEngine::instance()
{
	static InterpreterEngine eng;
	return eng;
}

InterpreterEngine::InterpreterEngine()
{
	ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr &m) { on_ws_message(m); });
	/* 네트워크 전환 등으로 조용히 죽은 연결 감지 — pong 미수신이면 close 되어 자동 재연결이 트리거된다 */
	ws_.setPingInterval(5);
	running_ = true;
	mix_worker_ = std::thread(&InterpreterEngine::mix_worker_fn, this);
	ws_worker_ = std::thread(&InterpreterEngine::ws_worker_fn, this);
}

InterpreterEngine::~InterpreterEngine()
{
	shutdown();
}

/* ───────────────── 설정 ───────────────── */
void InterpreterEngine::configure(const std::string &url, const std::string &key, const std::string &eng,
				  const std::string &speaker, bool voice_conversion)
{
	{
		std::lock_guard<std::mutex> lk(cfg_mtx_);
		server_url_ = url;
		service_key_ = key;
		engine_ = (eng == "openai") ? "openai" : "gemini";
		speaker_ = speaker;
		voice_conversion_ = voice_conversion;
	}
	if (enabled_.load()) {
		apply_engine_rate();
		ws_.stop();
		ws_ready_ = false;
		clear_conn_error(); /* 설정 변경 → 새 시도, 이전 실패 사유 제거 */
		rebuild_ws_url();
		ws_.start();
	}
	save_config();
}

std::string InterpreterEngine::server_url()
{
	std::lock_guard<std::mutex> lk(cfg_mtx_);
	return server_url_;
}
std::string InterpreterEngine::service_key()
{
	std::lock_guard<std::mutex> lk(cfg_mtx_);
	return service_key_;
}
std::string InterpreterEngine::engine()
{
	std::lock_guard<std::mutex> lk(cfg_mtx_);
	return engine_;
}
std::string InterpreterEngine::speaker()
{
	std::lock_guard<std::mutex> lk(cfg_mtx_);
	return speaker_;
}
bool InterpreterEngine::voice_conversion()
{
	std::lock_guard<std::mutex> lk(cfg_mtx_);
	return voice_conversion_;
}

void InterpreterEngine::apply_engine_rate()
{
	uint32_t r = (engine() == "openai") ? 24000u : 16000u;
	rate_ = r;
	chunk_bytes_ = static_cast<size_t>(r / 5); /* 100ms @ rate, s16 mono = rate/5 바이트 */
	struct obs_audio_info oai;
	if (obs_get_audio_info(&oai)) {
		obs_rate_ = oai.samples_per_sec;
		obs_channels_ = get_audio_channels(oai.speakers);
	}
	{
		std::lock_guard<std::mutex> rlk(rs_mtx_);
		if (resampler_) {
			audio_resampler_destroy(resampler_);
			resampler_ = nullptr;
		}
		struct resample_info src = {obs_rate_, AUDIO_FORMAT_FLOAT, SPEAKERS_MONO};
		struct resample_info dst = {r, AUDIO_FORMAT_16BIT, SPEAKERS_MONO};
		resampler_ = audio_resampler_create(&dst, &src);
	}
	mixer_reset();
	{
		std::lock_guard<std::mutex> blk(buf_mtx_);
		upload_buf_.clear();
	}
}

/* server_url_ 입력을 (scheme, host) 로 분리하고 끝의 '/' 제거.
 * 운영자는 폰 청취 페이지 주소(https://host)를 그대로 입력 → 플러그인이 wss/http base 로 변환한다. */
static void split_url(const std::string &url, std::string &scheme, std::string &host)
{
	auto p = url.find("://");
	if (p != std::string::npos) {
		scheme = url.substr(0, p);
		host = url.substr(p + 3);
	} else {
		scheme.clear();
		host = url;
	}
	while (!host.empty() && host.back() == '/')
		host.pop_back();
}

void InterpreterEngine::rebuild_ws_url()
{
	std::string url;
	{
		std::lock_guard<std::mutex> lk(cfg_mtx_);
		std::string scheme, host;
		split_url(server_url_, scheme, host);
		/* https/wss → wss(보안), 그 외(http/ws/스킴없음) → ws */
		const char *ws = (scheme == "https" || scheme == "wss") ? "wss://" : "ws://";
		url = ws + host + "/ingress?key=" + service_key_ + "&engine=" + engine_;
		if (voice_conversion_ && !speaker_.empty())
			url += "&speaker=" + speaker_; /* 음색 변환 ON 일 때만 → 서버가 speaker 유무로 토글 */
	}
	ws_.setUrl(url);
}

std::string InterpreterEngine::http_base()
{
	std::lock_guard<std::mutex> lk(cfg_mtx_);
	std::string scheme, host;
	split_url(server_url_, scheme, host);
	/* https/wss → https, 그 외 → http. 도크가 여기에 "/speakers?key=" 를 붙여 GET. */
	const char *http = (scheme == "https" || scheme == "wss") ? "https://" : "http://";
	return std::string(http) + host;
}

/* ───────────────── 선택 소스 ───────────────── */
void InterpreterEngine::set_selected_sources(const std::vector<std::string> &names)
{
	std::lock_guard<std::mutex> lk(src_mtx_);
	/* 제거: 기존 중 names 에 없는 것 */
	for (auto it = sources_.begin(); it != sources_.end();) {
		if (std::find(names.begin(), names.end(), it->first) == names.end()) {
			obs_source_t *s = obs_weak_source_get_source(it->second);
			if (s) {
				obs_source_remove_audio_capture_callback(s, on_source_audio, this);
				obs_source_release(s);
			}
			obs_weak_source_release(it->second);
			it = sources_.erase(it);
		} else {
			++it;
		}
	}
	/* 추가: names 중 신규 (이미 만들어진 소스만 — 로드 전이면 무시) */
	for (const auto &name : names) {
		if (sources_.count(name))
			continue;
		obs_source_t *s = obs_get_source_by_name(name.c_str());
		if (!s)
			continue;
		obs_source_add_audio_capture_callback(s, on_source_audio, this);
		sources_[name] = obs_source_get_weak_source(s);
		obs_source_release(s);
		obs_log(LOG_INFO, "[interpreter] 소스 탭 추가: %s", name.c_str());
	}
}

std::vector<std::string> InterpreterEngine::selected_sources()
{
	std::lock_guard<std::mutex> lk(src_mtx_);
	std::vector<std::string> v;
	for (auto &p : sources_)
		v.push_back(p.first);
	return v;
}

void InterpreterEngine::unregister_all_sources()
{
	std::lock_guard<std::mutex> lk(src_mtx_);
	for (auto &p : sources_) {
		obs_source_t *s = obs_weak_source_get_source(p.second);
		if (s) {
			obs_source_remove_audio_capture_callback(s, on_source_audio, this);
			obs_source_release(s);
		}
		obs_weak_source_release(p.second);
	}
	sources_.clear();
}

/* ───────────────── 캡처콜백 + 믹서 ───────────────── */
void InterpreterEngine::on_source_audio(void *param, obs_source_t *src, const struct audio_data *ad, bool muted)
{
	UNUSED_PARAMETER(src);
	auto *e = static_cast<InterpreterEngine *>(param);
	if (!e->enabled_.load() || !ad || ad->frames == 0)
		return;
	if (muted) {
		e->mixer_note_ts(ad->timestamp, ad->frames); /* 타임라인만 전진 */
		return;
	}
	if (!ad->data[0])
		return;
	uint32_t ch = e->obs_channels_ ? e->obs_channels_ : 1;
	static thread_local std::vector<float> mono;
	mono.resize(ad->frames);
	for (uint32_t i = 0; i < ad->frames; i++) {
		float s = 0.f;
		uint32_t cnt = 0;
		for (uint32_t c = 0; c < ch; c++) {
			if (ad->data[c]) {
				s += reinterpret_cast<const float *>(ad->data[c])[i];
				cnt++;
			}
		}
		mono[i] = cnt ? s / static_cast<float>(cnt) : 0.f;
	}
	e->mixer_add(ad->timestamp, mono.data(), ad->frames);
}

void InterpreterEngine::mixer_add(uint64_t ts, const float *mono, uint32_t frames)
{
	std::lock_guard<std::mutex> lk(mix_mtx_);
	if (base_ts_ == 0) {
		base_ts_ = ts;
		newest_ts_ = ts;
	}
	int64_t off_ns = static_cast<int64_t>(ts - base_ts_);
	if (off_ns < -1000000000LL || off_ns > 2000000000LL) { /* 큰 점프 → 리셋 */
		accum_.clear();
		base_ts_ = ts;
		newest_ts_ = ts;
		off_ns = 0;
	}
	int64_t off_frames = off_ns * static_cast<int64_t>(obs_rate_) / 1000000000LL;
	if (off_frames < 0) { /* 과거 → 앞 trim */
		uint32_t skip = static_cast<uint32_t>(std::min<int64_t>(-off_frames, frames));
		if (skip >= frames)
			return;
		mono += skip;
		frames -= skip;
		off_frames = 0;
	}
	size_t need = static_cast<size_t>(off_frames) + frames;
	if (accum_.size() < need)
		accum_.resize(need, 0.f);
	for (uint32_t i = 0; i < frames; i++)
		accum_[static_cast<size_t>(off_frames) + i] += mono[i];
	uint64_t end_ts = ts + static_cast<uint64_t>(frames) * NS / obs_rate_;
	if (end_ts > newest_ts_)
		newest_ts_ = end_ts;
}

void InterpreterEngine::mixer_note_ts(uint64_t ts, uint32_t frames)
{
	std::lock_guard<std::mutex> lk(mix_mtx_);
	if (base_ts_ == 0) {
		base_ts_ = ts;
		newest_ts_ = ts;
	}
	uint64_t end_ts = ts + static_cast<uint64_t>(frames) * NS / obs_rate_;
	if (end_ts > newest_ts_)
		newest_ts_ = end_ts;
}

void InterpreterEngine::mixer_reset()
{
	std::lock_guard<std::mutex> lk(mix_mtx_);
	accum_.clear();
	accum_.reserve(static_cast<size_t>(obs_rate_) / 2); /* ~0.5s 여유 → 안정상태 reallocation 제거 */
	base_ts_ = 0;
	newest_ts_ = 0;
}

/* ───────────────── 워커 ───────────────── */
void InterpreterEngine::mix_worker_fn()
{
	using namespace std::chrono_literals;
	std::vector<float> chunk;
	while (running_.load()) {
		std::this_thread::sleep_for(20ms);
		size_t n = 0;
		chunk.clear();
		{
			std::lock_guard<std::mutex> lk(mix_mtx_);
			if (base_ts_ != 0 && newest_ts_ > base_ts_ + MIX_WINDOW_NS) {
				uint64_t emit_ns = (newest_ts_ - MIX_WINDOW_NS) - base_ts_;
				n = static_cast<size_t>(emit_ns * obs_rate_ / NS);
				if (n > accum_.size())
					n = accum_.size();
				if (n > 0) {
					chunk.assign(accum_.begin(), accum_.begin() + n);
					accum_.erase(accum_.begin(), accum_.begin() + n);
					base_ts_ += static_cast<uint64_t>(n) * NS / obs_rate_;
				}
			}
		}
		if (n == 0)
			continue;
		for (auto &v : chunk) /* 합산 클리핑 방지 */
			v = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
		std::lock_guard<std::mutex> rlk(rs_mtx_);
		if (!resampler_)
			continue;
		uint8_t *out[MAX_AV_PLANES] = {0};
		uint32_t out_frames = 0;
		uint64_t ts_off = 0;
		const uint8_t *in[MAX_AV_PLANES] = {0};
		in[0] = reinterpret_cast<const uint8_t *>(chunk.data());
		if (audio_resampler_resample(resampler_, out, &out_frames, &ts_off, in,
					     static_cast<uint32_t>(chunk.size()))) {
			size_t bytes = static_cast<size_t>(out_frames) * 2; /* s16 mono */
			if (bytes && out[0]) {
				std::lock_guard<std::mutex> blk(buf_mtx_);
				upload_buf_.insert(upload_buf_.end(), out[0], out[0] + bytes);
				if (upload_buf_.size() >= chunk_bytes_.load())
					buf_cv_.notify_one();
			}
		}
	}
}

void InterpreterEngine::ws_worker_fn()
{
	std::vector<uint8_t> chunk;
	while (running_.load()) {
		std::unique_lock<std::mutex> lk(buf_mtx_);
		buf_cv_.wait(lk, [this] { return !running_.load() || upload_buf_.size() >= chunk_bytes_.load(); });
		size_t cb = chunk_bytes_.load();
		while (upload_buf_.size() >= cb) {
			chunk.assign(upload_buf_.begin(), upload_buf_.begin() + cb);
			upload_buf_.erase(upload_buf_.begin(), upload_buf_.begin() + cb);
			lk.unlock();
			if (enabled_.load() && ws_ready_.load())
				ws_.sendBinary(std::string(reinterpret_cast<const char *>(chunk.data()), chunk.size()));
			lk.lock();
			cb = chunk_bytes_.load();
		}
	}
}

/* ───────────────── WebSocket / 상태 ───────────────── */
void InterpreterEngine::on_ws_message(const ix::WebSocketMessagePtr &msg)
{
	switch (msg->type) {
	case ix::WebSocketMessageType::Open:
		ws_ready_ = true;
		clear_conn_error(); /* 연결 성공 → 이전 실패 사유 제거 */
		obs_log(LOG_INFO, "[interpreter] 서버 연결됨");
		break;
	case ix::WebSocketMessageType::Close: {
		ws_ready_ = false;
		uint16_t code = msg->closeInfo.code;
		/* 정상 종료(stop()/1000/1001)는 에러로 취급하지 않음. 인증 거부 코드(4401)만 사유 저장. */
		if (code == 4401)
			set_conn_error(CONN_ERR_AUTH);
		obs_log(LOG_INFO, "[interpreter] 서버 연결 끊김 (code=%u %s)", code,
			msg->closeInfo.reason.c_str());
		break;
	}
	case ix::WebSocketMessageType::Error: {
		ws_ready_ = false;
		int st = msg->errorInfo.http_status;
		const std::string &reason = msg->errorInfo.reason;
		if (st == 401 || st == 403 || st == 4401)
			set_conn_error(CONN_ERR_AUTH);
		else if (st > 0)
			set_conn_error(CONN_ERR_HTTP, st);
		else
			set_conn_error(CONN_ERR_NETWORK);
		obs_log(LOG_WARNING, "[interpreter] WS 오류: status=%d reason=%s", st, reason.c_str());
		break;
	}
	case ix::WebSocketMessageType::Message:
		if (!msg->binary)
			parse_status(msg->str);
		break;
	default:
		break;
	}
}

void InterpreterEngine::set_conn_error(int kind, int http)
{
	std::lock_guard<std::mutex> lk(conn_mtx_);
	conn_err_kind_ = kind;
	conn_err_http_ = http;
}

void InterpreterEngine::clear_conn_error()
{
	std::lock_guard<std::mutex> lk(conn_mtx_);
	conn_err_kind_ = CONN_ERR_NONE;
	conn_err_http_ = 0;
}

int InterpreterEngine::connection_error_kind()
{
	std::lock_guard<std::mutex> lk(conn_mtx_);
	return conn_err_kind_;
}

int InterpreterEngine::connection_error_http()
{
	std::lock_guard<std::mutex> lk(conn_mtx_);
	return conn_err_http_;
}

void InterpreterEngine::parse_status(const std::string &text)
{
	try {
		auto j = nlohmann::json::parse(text);
		if (j.value("type", std::string()) != "status")
			return;
		ServerStatus s;
		s.live = j.value("live", false);
		s.engine = j.value("engine", std::string());
		s.durationSec = j.value("durationSec", 0);
		s.total = j.value("total", 0);
		if (j.contains("listeners") && j["listeners"].is_object())
			for (auto &kv : j["listeners"].items())
				s.listeners[kv.key()] = kv.value().get<int>();
		std::lock_guard<std::mutex> lk(status_mtx_);
		status_ = std::move(s);
	} catch (...) {
		/* preview 서버 — 형식 불일치 무시 */
	}
}

ServerStatus InterpreterEngine::status()
{
	std::lock_guard<std::mutex> lk(status_mtx_);
	return status_;
}

/* ───────────────── 토글 / 상태 ───────────────── */
void InterpreterEngine::start()
{
	apply_engine_rate();
	enabled_ = true;
	ws_.stop();
	ws_ready_ = false;
	clear_conn_error(); /* 새 시도 → 이전 실패 사유 제거 (INTERP_CONNECTING 부터 시작) */
	rebuild_ws_url();
	ws_.start();
	save_config();
	obs_log(LOG_INFO, "[interpreter] 통역 시작 (engine=%s)", engine().c_str());
}

void InterpreterEngine::stop()
{
	enabled_ = false;
	ws_.stop();
	ws_ready_ = false;
	{
		std::lock_guard<std::mutex> lk(status_mtx_);
		status_ = ServerStatus{};
	}
	save_config();
	obs_log(LOG_INFO, "[interpreter] 통역 중지");
}

void InterpreterEngine::toggle()
{
	if (enabled_.load())
		stop();
	else
		start();
}

int InterpreterEngine::state()
{
	{
		std::lock_guard<std::mutex> lk(src_mtx_);
		if (sources_.empty())
			return INTERP_NONE;
	}
	if (!enabled_.load())
		return INTERP_OFF;
	if (ws_ready_.load())
		return INTERP_LIVE;
	{
		std::lock_guard<std::mutex> lk(conn_mtx_);
		if (conn_err_kind_ != CONN_ERR_NONE)
			return INTERP_ERROR; /* 연결 실패 사유 있음 → 시도 중과 구분 */
	}
	return INTERP_CONNECTING;
}

/* ───────────────── 영속성 ───────────────── */
void InterpreterEngine::save_config()
{
	obs_data_t *d = obs_data_create();
	{
		std::lock_guard<std::mutex> lk(cfg_mtx_);
		obs_data_set_string(d, "server_url", server_url_.c_str());
		obs_data_set_string(d, "service_key", service_key_.c_str()); /* 키는 이 파일에만 */
		obs_data_set_string(d, "engine", engine_.c_str());
		obs_data_set_string(d, "speaker", speaker_.c_str());
		obs_data_set_bool(d, "voice_conversion", voice_conversion_);
	}
	obs_data_set_bool(d, "enabled", enabled_.load());
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &name : selected_sources()) {
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "name", name.c_str());
		obs_data_array_push_back(arr, it);
		obs_data_release(it);
	}
	obs_data_set_array(d, "sources", arr);
	obs_data_array_release(arr);

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("interpreter.json");
	if (path) {
		obs_data_save_json(d, path);
		bfree(path);
	}
	obs_data_release(d);
}

void InterpreterEngine::load_config()
{
	char *path = obs_module_config_path("interpreter.json");
	obs_data_t *d = path ? obs_data_create_from_json_file(path) : nullptr;
	if (path)
		bfree(path);
	if (!d)
		return;
	{
		std::lock_guard<std::mutex> lk(cfg_mtx_);
		server_url_ = obs_data_get_string(d, "server_url");
		service_key_ = obs_data_get_string(d, "service_key");
		std::string eng = obs_data_get_string(d, "engine");
		engine_ = (eng == "openai") ? "openai" : "gemini";
		speaker_ = obs_data_get_string(d, "speaker"); /* 빈값 허용 — 서버 목록 받은 뒤 도크가 선택 */
		voice_conversion_ = obs_data_get_bool(d, "voice_conversion");
		if (server_url_.empty())
			server_url_ = "ws://localhost:8000";
	}
	std::vector<std::string> names;
	obs_data_array_t *arr = obs_data_get_array(d, "sources");
	if (arr) {
		size_t cnt = obs_data_array_count(arr);
		for (size_t i = 0; i < cnt; i++) {
			obs_data_t *it = obs_data_array_item(arr, i);
			names.push_back(obs_data_get_string(it, "name"));
			obs_data_release(it);
		}
		obs_data_array_release(arr);
	}
	bool en = obs_data_get_bool(d, "enabled");
	obs_data_release(d);

	apply_engine_rate();
	set_selected_sources(names); /* 소스 존재해야 등록됨 → FINISHED_LOADING 에서 호출 */
	if (en)
		start();
}

/* ───────────────── 생명주기 ───────────────── */
void InterpreterEngine::shutdown()
{
	enabled_ = false;
	bool was_running = running_.exchange(false);
	ws_.stop();
	ws_ready_ = false;
	unregister_all_sources();
	buf_cv_.notify_all();
	if (was_running) {
		if (mix_worker_.joinable())
			mix_worker_.join();
		if (ws_worker_.joinable())
			ws_worker_.join();
	}
	std::lock_guard<std::mutex> rlk(rs_mtx_);
	if (resampler_) {
		audio_resampler_destroy(resampler_);
		resampler_ = nullptr;
	}
}
