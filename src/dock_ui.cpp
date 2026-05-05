#include "dock_ui.h"
#include "config_io.h"
#include "plugin_state.h"

#include "destination_rules.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QSet>
#include <QSignalBlocker>
#include <QDockWidget>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <string>

#ifndef PLUGIN_UI_VERSION_STRING
#define PLUGIN_UI_VERSION_STRING "v0.0.0-dev"
#endif

namespace {

QString tr_ms(const char *id)
{
    return QString::fromUtf8(obs_module_text(id));
}

void populate_video_encoder_combo(QComboBox *cb, PlatformKind platform_kind)
{
    cb->clear();
    const std::set<std::string> allowed = allowed_codecs_for_platform(platform_kind);
    QSet<QString> seen_display;

    for (size_t i = 0;; ++i) {
        const char *enc_id = nullptr;
        if (!obs_enum_encoder_types(i, &enc_id)) {
            break;
        }
        if (obs_get_encoder_type(enc_id) != OBS_ENCODER_VIDEO) {
            continue;
        }
        if (!allowed.empty()) {
            const char *codec = obs_get_encoder_codec(enc_id);
            if (!codec || allowed.find(codec) == allowed.end()) {
                continue;
            }
        }
        const char *disp = obs_encoder_get_display_name(enc_id);
        const QString label = disp ? QString::fromUtf8(disp) : QString::fromUtf8(enc_id);
        const QString haystack = (QString::fromUtf8(enc_id) + QLatin1Char(' ') + label).toLower();
        if (haystack.contains(QStringLiteral("deprecated")) || haystack.contains(QStringLiteral("fallback")) ||
            haystack.contains(QStringLiteral("obsolete"))) {
            continue;
        }
        const QString display_key = label.trimmed().toLower();
        if (seen_display.contains(display_key)) {
            continue;
        }
        seen_display.insert(display_key);
        cb->addItem(label, QString::fromUtf8(enc_id));
    }
    if (cb->count() == 0) {
        cb->addItem(QStringLiteral("Software (x264)"), QStringLiteral("obs_x264"));
    }
}

void select_video_encoder_combo(QComboBox *cb, const std::string &saved_id)
{
    if (!saved_id.empty()) {
        const int ix = cb->findData(QString::fromStdString(saved_id));
        if (ix >= 0) {
            cb->setCurrentIndex(ix);
            return;
        }
    }
    const int fallback = cb->findData(QStringLiteral("obs_x264"));
    if (fallback >= 0) {
        cb->setCurrentIndex(fallback);
        return;
    }
    if (cb->count() > 0) {
        cb->setCurrentIndex(0);
    }
}

QString encoder_display_label(const Destination &dst)
{
    std::string id = dst.video_encoder_id;
    if (id.empty() && dst.is_default) {
        obs_output_t *out = obs_frontend_get_streaming_output();
        if (out) {
            obs_encoder_t *enc = obs_output_get_video_encoder(out);
            if (enc) {
                const char *eid = obs_encoder_get_id(enc);
                if (eid) id = eid;
            }
            obs_output_release(out);
        }
        if (id.empty()) {
            config_t *config = obs_frontend_get_profile_config();
            if (config) {
                const char *mode = config_get_string(config, "Output", "Mode");
                const char *enc_id = nullptr;
                if (mode && strcmp(mode, "Advanced") == 0) {
                    enc_id = config_get_string(config, "AdvOut", "Encoder");
                } else {
                    enc_id = config_get_string(config, "SimpleOutput", "StreamEncoder");
                }
                if (enc_id && enc_id[0] != '\0') id = enc_id;
            }
        }
    }
    if (id.empty()) {
        return QStringLiteral("Software (x264)");
    }
    const char *disp = obs_encoder_get_display_name(id.c_str());
    return disp ? QString::fromUtf8(disp) : QString::fromStdString(id);
}

void apply_platform_row_to_combo(QComboBox *platform, const std::string &plat_str)
{
    const QString qid = QString::fromStdString(plat_str);
    const int ix = platform->findData(qid);
    if (ix >= 0) {
        platform->setCurrentIndex(ix);
        return;
    }
    platform->addItem(qid, qid);
    platform->setCurrentIndex(platform->count() - 1);
}

static std::string runtime_state_ui(RuntimeState s)
{
    switch (s) {
    case RuntimeState::Idle:
        return obs_module_text("StatusIdle");
    case RuntimeState::Starting:
        return obs_module_text("StatusStarting");
    case RuntimeState::Live:
        return obs_module_text("StatusLive");
    case RuntimeState::Failed:
        return obs_module_text("StatusFailed");
    case RuntimeState::Stopped:
        return obs_module_text("StatusStopped");
    default:
        return obs_module_text("StatusUnknown");
    }
}

std::string status_text_for_table(const RuntimeStatus &status, bool enabled)
{
    if (!enabled) {
        return obs_module_text("StatusDisabled");
    }

    std::string value = runtime_state_ui(status.state);
    if (status.retry_max > 0 && (status.state == RuntimeState::Starting || status.state == RuntimeState::Failed)) {
        const QString r = QString::fromUtf8(obs_module_text("StatusRetryFmt"))
                              .arg(status.retry_attempt)
                              .arg(status.retry_max);
        value += r.toStdString();
    }
    return value;
}

QColor status_color_for_table(const RuntimeStatus &status, bool enabled)
{
    if (!enabled) {
        return QColor(160, 160, 160);
    }

    switch (status.state) {
    case RuntimeState::Live:
        return QColor(0, 170, 0);
    case RuntimeState::Failed:
        return QColor(200, 40, 40);
    case RuntimeState::Starting:
        return QColor(210, 140, 0);
    case RuntimeState::Stopped:
    case RuntimeState::Idle:
    default:
        return QColor(200, 200, 200);
    }
}

std::string error_text_for_table(const RuntimeStatus &status)
{
    if (!status.terminal_reason.empty()) {
        if (status.last_error.empty() || status.last_error == status.terminal_reason) {
            return status.terminal_reason;
        }
        return status.last_error + std::string(obs_module_text("ErrorTerminalSep")) + status.terminal_reason;
    }
    return status.last_error;
}

struct DockChromeState {
    QPushButton *btn_add = nullptr;
    QPushButton *btn_edit = nullptr;
    QPushButton *btn_remove = nullptr;
};

DockChromeState g_chrome;

void dock_update_action_buttons()
{
    if (!g_chrome.btn_edit || !g_destinations_table) {
        return;
    }

    const int r = g_destinations_table->currentRow();
    const bool has = r >= 0 && r < static_cast<int>(g_destinations.size());
    const bool can_edit_non_default = has && !g_destinations[static_cast<size_t>(r)].is_default;
    const bool live = obs_frontend_streaming_active();
    const bool edit_remove_enabled = can_edit_non_default && !live;

    g_chrome.btn_edit->setEnabled(edit_remove_enabled);
    g_chrome.btn_add->setEnabled(true);
    g_chrome.btn_remove->setEnabled(edit_remove_enabled);
    if (live) {
        const QString tip = tr_ms("OpsLockedWhileLive");
        g_chrome.btn_edit->setToolTip(tip);
        g_chrome.btn_remove->setToolTip(tip);
    } else {
        g_chrome.btn_edit->setToolTip(QString());
        g_chrome.btn_remove->setToolTip(QString());
    }
}

/** Modal dialog with full destination form. Returns true if user saved valid data into *out. */
bool open_destination_editor_dialog(QWidget *parent, const Destination &initial, Destination *out, int skip_duplicate_index,
                                    bool is_add)
{
    if (!out) {
        return false;
    }

    QDialog dlg(parent);
    dlg.setModal(true);
    dlg.setWindowTitle(is_add ? tr_ms("DlgAddDestinationTitle") : tr_ms("DlgEditDestinationTitle"));
    dlg.setMinimumWidth(420);

    auto *vl = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout();

    auto *platform = new QComboBox(&dlg);
    if (is_add) {
        platform->insertItem(0, tr_ms("PlatformChoosePlaceholder"), QVariant());
    }
    platform->addItem(tr_ms("PlatformYouTube"), QStringLiteral("YouTube"));
    platform->addItem(tr_ms("PlatformTwitch"), QStringLiteral("Twitch"));
    platform->addItem(tr_ms("PlatformKick"), QStringLiteral("Kick"));
    platform->addItem(tr_ms("PlatformOther"), QStringLiteral("Other"));
    auto *server = new QLineEdit(&dlg);
    server->setReadOnly(true);
    auto *stream_key = new QLineEdit(&dlg);
    stream_key->setEchoMode(QLineEdit::Password);
    auto *video_encoder = new QComboBox(&dlg);

    form->addRow(tr_ms("LabelPlatform"), platform);
    form->addRow(tr_ms("LabelServer"), server);
    form->addRow(tr_ms("LabelStreamKey"), stream_key);
    form->addRow(tr_ms("LabelVideoEncoder"), video_encoder);
    vl->addLayout(form);

    std::function<void()> sync_platform_dependent_ui = [platform, server, video_encoder]() {
        const QVariant raw = platform->currentData();
        if (!raw.isValid() || raw.toString().isEmpty()) {
            server->clear();
            server->setReadOnly(true);
            server->setPlaceholderText(tr_ms("ServerChoosePlatformHint"));
            video_encoder->clear();
            return;
        }
        server->setPlaceholderText(QString());
        const QString id = raw.toString();
        const PlatformKind kind = detect_platform_kind(id.toStdString());
        const bool preset = (id == QStringLiteral("YouTube") || id == QStringLiteral("Twitch") ||
                             id == QStringLiteral("Kick"));
        server->setReadOnly(preset);
        if (preset) {
            server->setText(QString::fromStdString(default_server_for_platform(kind)));
        } else {
            server->clear();
            server->setReadOnly(false);
        }
        const QString prev_enc = video_encoder->currentData().toString();
        populate_video_encoder_combo(video_encoder, kind);
        select_video_encoder_combo(video_encoder, prev_enc.toStdString());
    };

    if (is_add) {
        platform->setCurrentIndex(0);
    } else {
        apply_platform_row_to_combo(platform, initial.platform);
    }
    sync_platform_dependent_ui();
    if (!initial.server.empty()) {
        server->setText(QString::fromStdString(initial.server));
    }
    stream_key->setText(QString::fromStdString(initial.stream_key));
    select_video_encoder_combo(video_encoder, initial.video_encoder_id);

    QObject::connect(platform, &QComboBox::currentIndexChanged,
                     [sync_platform_dependent_ui](int) { sync_platform_dependent_ui(); });

    auto *btn_row = new QHBoxLayout();
    btn_row->addStretch(1);
    auto *btn_cancel = new QPushButton(tr_ms("BtnCancel"), &dlg);
    auto *btn_save = new QPushButton(tr_ms("BtnSave"), &dlg);
    btn_save->setDefault(true);
    btn_row->addWidget(btn_cancel);
    btn_row->addWidget(btn_save);
    vl->addLayout(btn_row);

    QObject::connect(btn_cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(btn_save, &QPushButton::clicked, [&]() {
        const QVariant pdata = platform->currentData();
        if (is_add && (!pdata.isValid() || pdata.toString().isEmpty())) {
            QMessageBox::warning(&dlg, tr_ms("DlgAddDestinationTitle"), tr_ms("ErrPickPlatform"));
            return;
        }

        Destination updated = initial;
        updated.platform = pdata.toString().toStdString();
        if (updated.platform.empty()) {
            updated.platform = platform->currentText().toStdString();
        }
        updated.server = server->text().toStdString();
        updated.stream_key = stream_key->text().toStdString();
        updated.video_encoder_id = video_encoder->currentData().toString().toStdString();

        normalize_destination(updated);
        if (!validate_and_log_destination(updated, skip_duplicate_index)) {
            return;
        }
        *out = std::move(updated);
        dlg.accept();
    });

    return dlg.exec() == QDialog::Accepted;
}

} // namespace

void refresh_destinations_table()
{
    if (!g_destinations_table) {
        return;
    }

    const int row_to_restore = g_destinations_table->currentRow();

    g_is_refreshing_table = true;
    std::unordered_map<std::string, RuntimeStatus> status_snapshot;
    {
        std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
        status_snapshot = g_runtime_statuses;
    }

    const bool streaming_active = obs_frontend_streaming_active();

    g_destinations_table->setRowCount(static_cast<int>(g_destinations.size()));
    for (int row = 0; row < static_cast<int>(g_destinations.size()); ++row) {
        const Destination &dst = g_destinations[static_cast<size_t>(row)];
        const auto it = status_snapshot.find(destination_id(dst));
        const RuntimeStatus runtime = it == status_snapshot.end() ? RuntimeStatus{} : it->second;

        const QString platform_label =
            dst.is_default ? QString::fromStdString(dst.platform) + tr_ms("ObsDefaultLockSuffix")
                           : QString::fromStdString(dst.platform);

        auto *enabled_item = new QTableWidgetItem();
        auto *platform_item = new QTableWidgetItem(platform_label);
        auto *server_item = new QTableWidgetItem(QString::fromStdString(dst.server));
        auto *encoder_item = new QTableWidgetItem(encoder_display_label(dst));
        auto *status_item = new QTableWidgetItem(QString::fromStdString(status_text_for_table(runtime, dst.enabled)));
        auto *error_item = new QTableWidgetItem(QString::fromStdString(error_text_for_table(runtime)));

        Qt::ItemFlags enabled_flags = (enabled_item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable;
        if (dst.is_default) {
            enabled_flags &= ~Qt::ItemIsUserCheckable;
            enabled_flags &= ~Qt::ItemIsEnabled;
        } else if (streaming_active && dst.enabled) {
            /* Avoid disabling outputs mid-stream (can break RTMP / OBS pipeline). */
            enabled_flags &= ~Qt::ItemIsEnabled;
            enabled_item->setToolTip(tr_ms("EnabledLockWhileLiveTip"));
        }
        enabled_item->setFlags(enabled_flags);
        enabled_item->setCheckState((dst.enabled || dst.is_default) ? Qt::Checked : Qt::Unchecked);

        if (dst.is_default) {
            QFont locked_font = platform_item->font();
            locked_font.setItalic(true);
            platform_item->setFont(locked_font);
            server_item->setFont(locked_font);
            const QString tooltip = tr_ms("ObsDefaultStreamTooltip");
            platform_item->setToolTip(tooltip);
            server_item->setToolTip(tooltip);
            enabled_item->setToolTip(tooltip);
        }
        platform_item->setFlags(platform_item->flags() & ~Qt::ItemIsEditable);
        server_item->setFlags(server_item->flags() & ~Qt::ItemIsEditable);
        encoder_item->setFlags(encoder_item->flags() & ~Qt::ItemIsEditable);
        status_item->setFlags(status_item->flags() & ~Qt::ItemIsEditable);
        error_item->setFlags(error_item->flags() & ~Qt::ItemIsEditable);
        status_item->setForeground(status_color_for_table(runtime, dst.enabled));

        g_destinations_table->setItem(row, 0, enabled_item);
        g_destinations_table->setItem(row, 1, platform_item);
        g_destinations_table->setItem(row, 2, server_item);
        g_destinations_table->setItem(row, 3, encoder_item);
        g_destinations_table->setItem(row, 4, status_item);
        g_destinations_table->setItem(row, 5, error_item);
    }

    g_destinations_table->resizeColumnsToContents();
    g_is_refreshing_table = false;

    if (row_to_restore >= 0 && row_to_restore < g_destinations_table->rowCount()) {
        const QSignalBlocker blocker(g_destinations_table);
        g_destinations_table->selectRow(row_to_restore);
    } else {
        const QSignalBlocker blocker(g_destinations_table);
        g_destinations_table->clearSelection();
    }

    dock_update_action_buttons();
}

void create_dock()
{
    auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main_window) {
        blog(LOG_WARNING, "[obs-multistream-plugin] Could not get OBS main window");
        return;
    }

    auto *container = new QWidget(main_window);
    auto *layout = new QVBoxLayout(container);

    auto *header_layout = new QHBoxLayout();
    header_layout->addStretch(1);
    auto *version_label = new QLabel(QString::fromUtf8(PLUGIN_UI_VERSION_STRING), container);
    version_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header_layout->addWidget(version_label);
    layout->addLayout(header_layout);

    auto *add_button = new QPushButton(tr_ms("BtnAddDestination"), container);
    auto *edit_button = new QPushButton(tr_ms("BtnEditSelected"), container);
    edit_button->setEnabled(false);
    auto *remove_button = new QPushButton(tr_ms("BtnRemoveSelected"), container);

    g_destinations_table = new QTableWidget(container);
    g_destinations_table->setColumnCount(6);
    g_destinations_table->setHorizontalHeaderLabels({tr_ms("ColEnabled"), tr_ms("ColPlatform"), tr_ms("ColServer"),
                                                     tr_ms("ColEncoder"), tr_ms("ColStatus"), tr_ms("ColLastError")});
    g_destinations_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    g_destinations_table->setSelectionMode(QAbstractItemView::SingleSelection);
    g_destinations_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    g_destinations_table->verticalHeader()->setVisible(false);
    g_destinations_table->horizontalHeader()->setStretchLastSection(true);
    g_destinations_table->setStyleSheet(
        QStringLiteral("QTableWidget::item:selected { background-color: rgb(160, 40, 40); color: rgb(255, 255, 255); }"
                       "QTableWidget::item:selected:!active { background-color: rgb(140, 35, 35); color: rgb(255, 255, 255); }"));

    g_chrome = DockChromeState{};
    g_chrome.btn_add = add_button;
    g_chrome.btn_edit = edit_button;
    g_chrome.btn_remove = remove_button;

    QObject::connect(add_button, &QPushButton::clicked, [main_window]() {
        Destination seed;
        seed.enabled = true;
        Destination result;
        if (!open_destination_editor_dialog(main_window, seed, &result, -1, true)) {
            return;
        }

        g_destinations.push_back(std::move(result));
        {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            g_runtime_statuses[destination_id(g_destinations.back())] = RuntimeStatus{};
        }
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination added");
    });

    QObject::connect(edit_button, &QPushButton::clicked, [main_window]() {
        if (!g_destinations_table) {
            return;
        }

        if (obs_frontend_streaming_active()) {
            QMessageBox::information(g_destinations_table->window(), tr_ms("BtnEditSelected"),
                                       tr_ms("OpsLockedWhileLive"));
            return;
        }

        const int row = g_destinations_table->currentRow();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No destination selected to edit");
            return;
        }

        if (g_destinations[static_cast<size_t>(row)].is_default) {
            blog(LOG_WARNING,
                 "[obs-multistream-plugin] The OBS default destination is read-only here; edit it in OBS "
                 "Settings -> Stream");
            return;
        }

        Destination seed = g_destinations[static_cast<size_t>(row)];
        Destination result;
        if (!open_destination_editor_dialog(main_window, seed, &result, row, false)) {
            return;
        }

        Destination &existing = g_destinations[static_cast<size_t>(row)];
        const std::string old_id = destination_id(existing);
        const std::string new_id = destination_id(result);

        RuntimeStatus status;
        {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            const auto status_it = g_runtime_statuses.find(old_id);
            if (status_it != g_runtime_statuses.end()) {
                status = status_it->second;
                g_runtime_statuses.erase(status_it);
            }
        }

        existing = std::move(result);

        {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            g_runtime_statuses[new_id] = status;
        }
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination edited for platform=%s", existing.platform.c_str());
    });

    QObject::connect(g_destinations_table, &QTableWidget::itemSelectionChanged, []() { dock_update_action_buttons(); });

    QObject::connect(g_destinations_table, &QTableWidget::itemChanged, [](QTableWidgetItem *item) {
        if (!item || !g_destinations_table || g_is_refreshing_table || item->column() != 0) {
            return;
        }

        const int row = item->row();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            return;
        }

        Destination &dst = g_destinations[static_cast<size_t>(row)];
        if (dst.is_default) {
            return;
        }
        const bool enabled = item->checkState() == Qt::Checked;
        if (obs_frontend_streaming_active() && dst.enabled && !enabled) {
            g_is_refreshing_table = true;
            item->setCheckState(Qt::Checked);
            g_is_refreshing_table = false;
            return;
        }
        if (dst.enabled == enabled) {
            return;
        }

        dst.enabled = enabled;
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination %s for platform=%s", enabled ? "enabled" : "disabled",
             dst.platform.c_str());
    });

    QObject::connect(remove_button, &QPushButton::clicked, []() {
        if (!g_destinations_table) {
            return;
        }

        if (obs_frontend_streaming_active()) {
            QMessageBox::information(g_destinations_table->window(), tr_ms("BtnRemoveSelected"),
                                       tr_ms("OpsLockedWhileLive"));
            return;
        }

        const int row = g_destinations_table->currentRow();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No destination selected to remove");
            return;
        }

        if (g_destinations[static_cast<size_t>(row)].is_default) {
            blog(LOG_WARNING,
                 "[obs-multistream-plugin] Cannot remove the OBS default destination; change it in OBS Settings -> Stream");
            return;
        }

        const QString plat = QString::fromStdString(g_destinations[static_cast<size_t>(row)].platform);
        /* DlgRemovePrompt uses %1; tr_ms() yields QString so .arg() substitutes (obs_module_text alone would not). */
        const QMessageBox::StandardButton answer =
            QMessageBox::question(g_destinations_table->window(), tr_ms("DlgRemoveTitle"),
                                  tr_ms("DlgRemovePrompt").arg(plat), QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }

        const std::string platform_name = g_destinations[static_cast<size_t>(row)].platform;
        {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            g_runtime_statuses.erase(destination_id(g_destinations[static_cast<size_t>(row)]));
        }
        g_destinations.erase(g_destinations.begin() + row);
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination removed for platform=%s", platform_name.c_str());
    });

    auto *manage_layout = new QHBoxLayout();
    manage_layout->addWidget(edit_button);
    manage_layout->addWidget(remove_button);

    layout->addWidget(add_button);
    layout->addWidget(g_destinations_table);
    layout->addLayout(manage_layout);
    layout->addStretch();

    refresh_destinations_table();
    if (g_destinations_table) {
        const QSignalBlocker blocker(g_destinations_table);
        g_destinations_table->clearSelection();
    }
    dock_update_action_buttons();

    g_dock = new QDockWidget(tr_ms("DockTitle"), main_window);
    g_dock->setObjectName("obs_multistream_destinations_dock");
    g_dock->setWidget(container);

    QObject::connect(g_dock, &QDockWidget::visibilityChanged, [](bool visible) {
        if (visible) {
            sync_default_destination_from_obs();
            refresh_destinations_table();
        }
    });

    main_window->addDockWidget(Qt::RightDockWidgetArea, g_dock);

    obs_frontend_add_tools_menu_item(obs_module_text("MenuItem"), [](void *) {
        if (!g_dock) {
            return;
        }
        g_dock->setVisible(true);
        g_dock->show();
        g_dock->raise();
        g_dock->activateWindow();
    }, nullptr);
}

void destroy_dock()
{
    if (!g_dock) {
        return;
    }

    g_dock->hide();
    g_dock->deleteLater();
    g_destinations_table = nullptr;
    g_dock = nullptr;
    g_chrome = DockChromeState{};
}
