#include "interpreter-dock.hpp"
#include "interpreter-control.hpp"
#include "interpreter-engine.hpp"

#include <obs.h>
#include <obs-module.h> /* obs_module_text — 로케일 문자열 조회 */

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>
#include <vector>

#include <QMetaObject>

#include <ixwebsocket/IXHttpClient.h>
#include <nlohmann/json.hpp>

/* 로케일 문자열 조회 헬퍼 — obs_module_text(키) → QString. en-US/ko-KR.ini 가 동일 키셋 보유. */
static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* obs_enum_sources 콜백 — 오디오 출력 플래그가 있는 소스 이름만 수집 */
static bool enum_audio_sources(void *p, obs_source_t *s)
{
	auto *names = static_cast<std::vector<QString> *>(p);
	if (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) {
		const char *n = obs_source_get_name(s);
		if (n && *n)
			names->push_back(QString::fromUtf8(n));
	}
	return true;
}

InterpreterDock::InterpreterDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(8);

	auto *lblServer = new QLabel(T("LabelServerUrl"), this);
	serverEdit = new QLineEdit(this);
	serverEdit->setPlaceholderText("https://your-host.example.com");
	auto *lblKey = new QLabel(T("LabelServiceKey"), this);
	keyEdit = new QLineEdit(this);
	keyEdit->setEchoMode(QLineEdit::Password);
	auto *lblEngine = new QLabel(T("LabelEngine"), this);
	engineBox = new QComboBox(this);
	engineBox->addItem(T("EngineGemini"), "gemini");
	engineBox->addItem(T("EngineOpenAI"), "openai");

	auto *lblVoice = new QLabel(T("LabelVoice"), this);
	voiceBox = new QCheckBox(T("VoiceConvert"), this);
	speakerBox = new QComboBox(this);
	speakerBox->setEnabled(false); /* 서버에서 목록 받기 전엔 비활성 — fetchSpeakers 가 채운다 */

	auto *lblSrc = new QLabel(T("LabelSources"), this);
	lblSrc->setWordWrap(true);
	sourceList = new QListWidget(this);
	sourceList->setMaximumHeight(150);

	status = new QLabel(this);
	status->setAlignment(Qt::AlignCenter);
	status->setWordWrap(true);
	monitor = new QLabel(this);
	monitor->setAlignment(Qt::AlignCenter);
	monitor->setWordWrap(true);
	monitor->setStyleSheet("color:#9aa0a6; font-size:12px;");

	button = new QPushButton(this);
	button->setMinimumHeight(48);

	layout->addWidget(lblServer);
	layout->addWidget(serverEdit);
	layout->addWidget(lblKey);
	layout->addWidget(keyEdit);
	layout->addWidget(lblEngine);
	layout->addWidget(engineBox);
	layout->addWidget(lblVoice);
	layout->addWidget(voiceBox);
	layout->addWidget(speakerBox);
	layout->addWidget(lblSrc);
	layout->addWidget(sourceList);
	layout->addWidget(status);
	layout->addWidget(monitor);
	layout->addWidget(button);
	layout->addStretch();

	connect(button, &QPushButton::clicked, this, &InterpreterDock::onToggle);
	connect(sourceList, &QListWidget::itemChanged, this, &InterpreterDock::onSourceItemChanged);
	/* 주소/키 확정 시 먼저 엔진에 저장(onSettingsChanged) → 그 값으로 설교자 목록 재조회(fetchSpeakers) */
	connect(serverEdit, &QLineEdit::editingFinished, this, &InterpreterDock::onSettingsChanged);
	connect(serverEdit, &QLineEdit::editingFinished, this, &InterpreterDock::fetchSpeakers);
	connect(keyEdit, &QLineEdit::editingFinished, this, &InterpreterDock::onSettingsChanged);
	connect(keyEdit, &QLineEdit::editingFinished, this, &InterpreterDock::fetchSpeakers);
	connect(engineBox, &QComboBox::currentIndexChanged, this, &InterpreterDock::onSettingsChanged);
	connect(speakerBox, &QComboBox::currentIndexChanged, this, &InterpreterDock::onSettingsChanged);
	connect(voiceBox, &QCheckBox::toggled, this, &InterpreterDock::onSettingsChanged);

	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &InterpreterDock::refresh);
	timer->start(500);

	reloadSettings();
	rebuildSourceList();
	refresh();
	/* fetchSpeakers() 는 여기서 호출하지 않는다 — 이 시점은 load_config 전이라 서버 주소가 비어 있어
	 * populateSpeakers→onSettingsChanged 가 빈 값으로 저장을 덮어쓴다. FINISHED_LOADING 의
	 * onConfigLoaded() 에서 load_config 후 호출한다. */
}

void InterpreterDock::reloadSettings()
{
	/* 프로그램적으로 채울 때 onSettingsChanged 가 안 튀게 신호 차단 */
	auto &e = InterpreterEngine::instance();
	serverEdit->blockSignals(true);
	serverEdit->setText(QString::fromStdString(e.server_url()));
	serverEdit->blockSignals(false);
	keyEdit->blockSignals(true);
	keyEdit->setText(QString::fromStdString(e.service_key()));
	keyEdit->blockSignals(false);
	engineBox->blockSignals(true);
	int idx = engineBox->findData(QString::fromStdString(e.engine()));
	if (idx >= 0)
		engineBox->setCurrentIndex(idx);
	engineBox->blockSignals(false);
	speakerBox->blockSignals(true);
	int sidx = speakerBox->findData(QString::fromStdString(e.speaker()));
	if (sidx >= 0)
		speakerBox->setCurrentIndex(sidx);
	speakerBox->blockSignals(false);
	voiceBox->blockSignals(true);
	voiceBox->setChecked(e.voice_conversion());
	voiceBox->blockSignals(false);
}

/* FINISHED_LOADING 에서 load_config 직후 1회 호출. 로드된 값으로 UI 를 채운 뒤 저장 가드를 풀고,
 * 그제서야 정상 서버 주소로 설교자 목록을 조회한다. (생성자에서 하면 load 전이라 빈 값이 저장됨) */
void InterpreterDock::onConfigLoaded()
{
	reloadSettings();
	rebuildSourceList();
	configReady_ = true; /* 이제부터 사용자/조회에 의한 변경은 정상 저장된다 */
	fetchSpeakers();     /* 로드된 server_url 로 /speakers 조회 */
}

void InterpreterDock::rebuildSourceList()
{
	buildingList = true;
	auto selected = InterpreterEngine::instance().selected_sources();
	sourceList->clear();
	std::vector<QString> names;
	obs_enum_sources(enum_audio_sources, &names);
	for (const auto &name : names) {
		auto *item = new QListWidgetItem(name, sourceList);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		bool on = std::find(selected.begin(), selected.end(), name.toStdString()) != selected.end();
		item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
	}
	buildingList = false;
}

void InterpreterDock::onSourceItemChanged(QListWidgetItem *)
{
	if (buildingList)
		return;
	std::vector<std::string> names;
	for (int i = 0; i < sourceList->count(); i++) {
		auto *it = sourceList->item(i);
		if (it->checkState() == Qt::Checked)
			names.push_back(it->text().toStdString());
	}
	InterpreterEngine::instance().set_selected_sources(names);
}

void InterpreterDock::onSettingsChanged()
{
	if (!configReady_)
		return; /* 설정 로드 전 — 빈 UI 값으로 기존 저장(server_url/service_key)을 덮어쓰지 않는다 */
	InterpreterEngine::instance().configure(serverEdit->text().toStdString(), keyEdit->text().toStdString(),
						engineBox->currentData().toString().toStdString(),
						speakerBox->currentData().toString().toStdString(),
						voiceBox->isChecked());
}

void InterpreterDock::onToggle()
{
	InterpreterEngine::instance().toggle();
	refresh();
}

void InterpreterDock::refresh()
{
	auto &eng = InterpreterEngine::instance();
	status->setStyleSheet(""); /* 매 갱신마다 리셋 — ERROR 빨강이 다른 상태로 남지 않게 */
	switch (eng.state()) {
	case INTERP_NONE:
		status->setText(T("StatusNoSource"));
		button->setText(T("BtnStart"));
		button->setEnabled(false);
		button->setStyleSheet("");
		break;
	case INTERP_OFF:
		status->setText(T("StatusStandby"));
		button->setText(T("BtnStart"));
		button->setEnabled(true);
		button->setStyleSheet("background:#2563eb; color:white; font-weight:bold; font-size:15px;");
		break;
	case INTERP_CONNECTING:
		status->setText(T("StatusConnecting"));
		button->setText(T("BtnStop"));
		button->setEnabled(true);
		button->setStyleSheet("background:#d97706; color:white; font-weight:bold; font-size:15px;");
		break;
	case INTERP_LIVE:
		status->setText(T("StatusLive"));
		button->setText(T("BtnStop"));
		button->setEnabled(true);
		button->setStyleSheet("background:#dc2626; color:white; font-weight:bold; font-size:15px;");
		break;
	case INTERP_ERROR: {
		QString reason;
		switch (eng.connection_error_kind()) {
		case CONN_ERR_AUTH:
			reason = T("ErrAuth");
			break;
		case CONN_ERR_NETWORK:
			reason = T("ErrNetwork");
			break;
		case CONN_ERR_HTTP:
			reason = T("ErrHttp").arg(eng.connection_error_http());
			break;
		default:
			reason = T("ErrAuth");
			break;
		}
		status->setStyleSheet("color:#dc2626; font-weight:bold;");
		status->setText(T("StatusError").arg(reason));
		button->setText(T("BtnStop"));
		button->setEnabled(true);
		button->setStyleSheet("background:#dc2626; color:white; font-weight:bold; font-size:15px;");
		break;
	}
	}

	/* 모니터링 패널 */
	ServerStatus st = InterpreterEngine::instance().status();
	if (st.live) {
		int s = st.durationSec;
		QString dur = QString::asprintf("%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
		QStringList parts;
		for (const auto &kv : st.listeners)
			parts << QString("%1 %2").arg(QString::fromStdString(kv.first)).arg(kv.second);
		QString br = parts.isEmpty() ? T("MonNoListeners")
					     : T("MonListeners").arg(st.total).arg(parts.join(" · "));
		monitor->setText(T("MonSummary").arg(dur, br, QString::fromStdString(st.engine)));
	} else {
		monitor->setText(T("MonWaiting"));
	}
}

/* ───────────────── 설교자 목록 (서버 /speakers 동적 조회) ───────────────── */
void InterpreterDock::fetchSpeakers()
{
	auto &eng = InterpreterEngine::instance();
	std::string server = eng.server_url();
	std::string url = eng.http_base() + "/speakers?key=" + eng.service_key();
	int gen = ++fetchGen_;

	if (server.empty()) { /* 주소 미입력 — 네트워크 시도 없이 안내만 */
		populateSpeakers({}, false, gen);
		return;
	}

	/* HTTP GET 은 워커 스레드에서(아키텍처 규칙: GUI/콜백 비블로킹). 결과는 GUI 스레드로 큐잉. */
	std::thread([this, url, gen]() {
		std::vector<std::pair<std::string, std::string>> list;
		bool ok = false;
		ix::HttpClient client(false);
		auto args = client.createRequest();
		args->connectTimeout = 5;
		args->transferTimeout = 5;
		auto resp = client.get(url, args);
		if (resp && resp->statusCode == 200) {
			auto j = nlohmann::json::parse(resp->body, nullptr, false);
			if (j.is_array()) {
				ok = true;
				for (const auto &e : j) {
					std::string k = e.value("key", std::string());
					std::string l = e.value("label", std::string());
					if (!k.empty())
						list.emplace_back(k, l.empty() ? k : l);
				}
			}
		}
		QMetaObject::invokeMethod(
			this, [this, list, ok, gen]() { populateSpeakers(list, ok, gen); }, Qt::QueuedConnection);
	}).detach();
}

void InterpreterDock::populateSpeakers(const std::vector<std::pair<std::string, std::string>> &list, bool ok,
				       int gen)
{
	if (gen != fetchGen_.load())
		return; /* 더 최신 요청이 진행 중 → 이 응답은 버린다 */

	auto &eng = InterpreterEngine::instance();
	std::string prev = eng.speaker(); /* 기존 선택 복원용 */
	bool has = !list.empty();

	speakerBox->blockSignals(true);
	voiceBox->blockSignals(true);
	speakerBox->clear();
	if (has) {
		for (const auto &p : list)
			speakerBox->addItem(QString::fromStdString(p.second), QString::fromStdString(p.first));
		int idx = speakerBox->findData(QString::fromStdString(prev));
		speakerBox->setCurrentIndex(idx >= 0 ? idx : 0);
	} else {
		QString msg = eng.server_url().empty() ? T("SpeakerNeedUrl")
			      : ok                       ? T("SpeakerNone")
							 : T("SpeakerFetchFail");
		speakerBox->addItem(msg); /* data 없음 → 선택해도 speaker 미설정 */
		voiceBox->setChecked(false);
	}
	speakerBox->setEnabled(has);
	voiceBox->setEnabled(has);
	speakerBox->blockSignals(false);
	voiceBox->blockSignals(false);

	onSettingsChanged(); /* 현재 speakerBox 선택 + voiceBox 상태를 엔진에 1회 반영 */
}
