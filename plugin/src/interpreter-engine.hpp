/*
Glossa — 통역 엔진 (싱글톤).

도크에서 체크한 오디오 소스들의 audio_capture_callback 을 받아 타임스탬프 정렬로 모노 믹싱 →
엔진 레이트(16k/24k)로 리샘플 → 단일 WebSocket 으로 서버 /ingress 에 업로드한다.
서버가 ingress 로 되돌려주는 상태(JSON)를 파싱해 도크 모니터링에 노출한다.

불변규칙: 캡처콜백은 const audio_data* (원본 불변), 콜백은 믹스 누산만(비블로킹),
리샘플·전송은 워커, service_key 는 설정 파일에만.
*/
#pragma once

#include "interpreter-control.hpp" /* interpreter_conn_error_t (멤버 기본값에 사용) */

#include <obs.h>
#include <media-io/audio-resampler.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* 서버가 ingress 로 보내는 모니터링 상태 */
struct ServerStatus {
	bool live = false;
	std::string engine;
	int durationSec = 0;
	int total = 0;
	std::map<std::string, int> listeners; /* lang -> count */
};

class InterpreterEngine {
public:
	static InterpreterEngine &instance();

	/* 설정 (도크에서 변경 → 영속화) */
	void configure(const std::string &server_url, const std::string &service_key, const std::string &engine,
		       const std::string &speaker, bool voice_conversion);
	std::string server_url();
	std::string http_base(); /* server_url → https://host or http://host (도크 /speakers 조회용) */
	std::string service_key();
	std::string engine();
	std::string speaker();
	bool voice_conversion();

	/* 선택 소스 (도크 체크박스) — 이름 목록. diff 해서 캡처콜백 등록/해제 */
	void set_selected_sources(const std::vector<std::string> &names);
	std::vector<std::string> selected_sources();

	/* 토글 / 상태 (interpreter-control 이 위임) */
	void start();
	void stop();
	bool is_enabled() const { return enabled_.load(); }
	int state(); /* INTERP_* */
	void toggle();

	/* 모니터링 (도크 폴링) */
	ServerStatus status();

	/* 마지막 연결 실패 사유 — "종류"만 노출(interpreter_conn_error_t). 문자열화는 도크가 로케일로.
	 * 연결 성공/정상이면 CONN_ERR_NONE. CONN_ERR_HTTP 일 때 HTTP 코드는 connection_error_http(). */
	int connection_error_kind();
	int connection_error_http();

	/* 영속성 (obs_module_config_path) */
	void load_config();
	void save_config();

	/* 생명주기 — module_unload / EXIT 에서 호출. 콜백 전부 remove + 워커 종료 */
	void shutdown();

	InterpreterEngine(const InterpreterEngine &) = delete;
	InterpreterEngine &operator=(const InterpreterEngine &) = delete;

private:
	InterpreterEngine();
	~InterpreterEngine();

	/* 캡처콜백 (OBS 오디오 스레드) */
	static void on_source_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted);
	void mixer_add(uint64_t ts, const float *mono, uint32_t frames);
	void mixer_note_ts(uint64_t ts, uint32_t frames);
	void mixer_reset();

	void apply_engine_rate();          /* engine 에 맞춰 rate/chunk/resampler 갱신 */
	void rebuild_ws_url();
	void unregister_all_sources();     /* 콜백 전부 remove (락 보유) */

	void mix_worker_fn();              /* 믹스 drain → 리샘플 → upload_buf */
	void ws_worker_fn();               /* upload_buf → ws.sendBinary (100ms 청크) */
	void on_ws_message(const ix::WebSocketMessagePtr &msg);
	void parse_status(const std::string &text);
	void set_conn_error(int kind, int http = 0); /* 연결 실패 사유 저장 (락 보유) */
	void clear_conn_error();                      /* 새 연결 시도 직전 초기화 */

	/* --- 설정 --- */
	std::mutex cfg_mtx_;
	std::string server_url_;
	std::string service_key_;
	std::string engine_{"gemini"};
	std::string speaker_;                    /* 설교자 voice key (음색 변환 ON, 빈값=미선택) */
	bool voice_conversion_{false};           /* 음색 변환 on/off */
	std::atomic<uint32_t> rate_{16000};      /* 엔진 입력 레이트 */
	std::atomic<size_t> chunk_bytes_{3200};  /* 100ms @ rate, s16 mono */

	/* --- 선택 소스 (name -> weak ref) --- */
	std::mutex src_mtx_;
	std::map<std::string, obs_weak_source_t *> sources_;

	/* --- 믹서 (48k mono float 누산 타임라인) --- */
	std::mutex mix_mtx_;
	uint32_t obs_rate_{48000};
	uint32_t obs_channels_{2};
	uint64_t base_ts_{0};
	uint64_t newest_ts_{0};
	std::vector<float> accum_;

	/* --- 리샘플 + 업로드 + 워커 --- */
	audio_resampler_t *resampler_ = nullptr;
	std::mutex rs_mtx_;
	std::mutex buf_mtx_;
	std::condition_variable buf_cv_;
	std::vector<uint8_t> upload_buf_;
	std::thread mix_worker_;
	std::thread ws_worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> enabled_{false};

	/* --- WebSocket --- */
	ix::WebSocket ws_;
	std::atomic<bool> ws_ready_{false};

	/* --- 서버 상태 --- */
	std::mutex status_mtx_;
	ServerStatus status_;

	/* --- 마지막 연결 실패 사유 종류 (Error/Close 이벤트 → 도크가 로케일로 문자열화) --- */
	std::mutex conn_mtx_;
	int conn_err_kind_{CONN_ERR_NONE};
	int conn_err_http_{0}; /* CONN_ERR_HTTP 일 때의 HTTP 상태코드 */
};
