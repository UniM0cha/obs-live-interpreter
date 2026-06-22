/*
Glossa — 모듈 진입점.

통역 입력은 더 이상 소스별 "필터"가 아니다. 통역 도크에서 체크한 오디오 소스들을
InterpreterEngine 이 audio_capture_callback 으로 받아 하나로 믹싱 → 단일 WS 로 서버에 보낸다.
여기서는 엔진 생성, 도크/전역 핫키/프론트엔드·소스 이벤트 등록만 한다.

불변규칙: 캡처콜백은 원본 불변(읽기전용), 콜백 비블로킹(엔진이 보장), 키는 설정 파일만.
*/
#include <obs-module.h>
#include <plugin-support.h>
#include <obs-frontend-api.h>

#include <ixwebsocket/IXNetSystem.h>

#include <QMetaObject>

#include "interpreter-control.hpp"
#include "interpreter-engine.hpp"
#include "interpreter-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static InterpreterDock *g_dock = nullptr;
static obs_hotkey_id g_toggle_hotkey = OBS_INVALID_HOTKEY_ID;

/* ── interpreter-control C API → 엔진 위임 (도크/핫키가 공유) ── */
int interpreter_state(void)
{
	return InterpreterEngine::instance().state();
}
void interpreter_toggle_all(void)
{
	InterpreterEngine::instance().toggle();
}

static void queue_rebuild()
{
	if (g_dock)
		QMetaObject::invokeMethod(g_dock, "rebuildSourceList", Qt::QueuedConnection);
}

/* 소스 생성/삭제/이름변경 → 도크 목록 재구성(GUI 스레드로 마샬링) */
static void on_source_signal(void *, calldata_t *)
{
	queue_rebuild();
}

static void on_frontend_event(enum obs_frontend_event ev, void *)
{
	switch (ev) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		InterpreterEngine::instance().load_config(); /* 이 시점에 소스 존재 → 콜백 등록됨 */
		if (g_dock) {
			QMetaObject::invokeMethod(g_dock, "reloadSettings", Qt::QueuedConnection);
			QMetaObject::invokeMethod(g_dock, "rebuildSourceList", Qt::QueuedConnection);
		}
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		queue_rebuild();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		InterpreterEngine::instance().shutdown();
		break;
	default:
		break;
	}
}

static void toggle_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		InterpreterEngine::instance().toggle();
}

bool obs_module_load(void)
{
	ix::initNetSystem();
	InterpreterEngine::instance(); /* 싱글톤 생성(믹스/WS 워커 시작) */
	obs_log(LOG_INFO, "Glossa 로드됨 (버전 %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	/* 프론트엔드 준비 후 도크 등록 (보기→도크 메뉴에 노출) */
	g_dock = new InterpreterDock();
	obs_frontend_add_dock_by_id("obs_live_interpreter_dock", obs_module_text("DockTitle"), g_dock);

	/* 전역 핫키 — 소스별이 아니라 통역 엔진 전체 토글 */
	g_toggle_hotkey = obs_hotkey_register_frontend("obs_live_interpreter.toggle", obs_module_text("ToggleHotkey"),
						       toggle_hotkey, nullptr);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	signal_handler_t *sh = obs_get_signal_handler();
	if (sh) {
		signal_handler_connect(sh, "source_create", on_source_signal, nullptr);
		signal_handler_connect(sh, "source_destroy", on_source_signal, nullptr);
		signal_handler_connect(sh, "source_rename", on_source_signal, nullptr);
	}

	obs_log(LOG_INFO, "[interpreter] 도크/핫키/이벤트 등록됨");
}

void obs_module_unload(void)
{
	signal_handler_t *sh = obs_get_signal_handler();
	if (sh) {
		signal_handler_disconnect(sh, "source_create", on_source_signal, nullptr);
		signal_handler_disconnect(sh, "source_destroy", on_source_signal, nullptr);
		signal_handler_disconnect(sh, "source_rename", on_source_signal, nullptr);
	}
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_toggle_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(g_toggle_hotkey);
	InterpreterEngine::instance().shutdown();
	ix::uninitNetSystem();
	obs_log(LOG_INFO, "Glossa 언로드됨");
}
