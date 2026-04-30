#include "dock_ui.h"
#include "plugin_state.h"

#include "destination_rules.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
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
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#ifndef PLUGIN_UI_VERSION_STRING
#define PLUGIN_UI_VERSION_STRING "v0.0.0-dev"
#endif

namespace {

QString tr_ms(const char *id)
{
    return QString::fromUtf8(obs_module_text(id));
}

QString encoder_cell_label(const Destination &dst)
{
    if (dst.video_encoder_id.empty()) {
        return tr_ms("EncoderDefault");
    }
    const char *disp = obs_encoder_get_display_name(dst.video_encoder_id.c_str());
    return disp ? QString::fromUtf8(disp) : QString::fromStdString(dst.video_encoder_id);
}

void populate_video_encoder_combo(QComboBox *cb)
{
    cb->clear();
    for (size_t i = 0;; ++i) {
        const char *enc_id = nullptr;
        if (!obs_enum_encoder_types(i, &enc_id)) {
            break;
        }
        if (obs_get_encoder_type(enc_id) != OBS_ENCODER_VIDEO) {
            continue;
        }
        const char *disp = obs_encoder_get_display_name(enc_id);
        const QString label = disp ? QString::fromUtf8(disp) : QString::fromUtf8(enc_id);
        cb->addItem(label, QString::fromUtf8(enc_id));
    }
    if (cb->count() == 0) {
        cb->addItem(tr_ms("SoftwareX264"), QStringLiteral("obs_x264"));
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
    } else if (cb->count() > 0) {
        cb->setCurrentIndex(0);
    }
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

void fill_destination_from_form(Destination &dst, QComboBox *video_enc, QSpinBox *video_br, QSpinBox *audio_br)
{
    dst.video_encoder_id = video_enc->currentData().toString().toStdString();
    dst.video_bitrate_kbps = video_br->value();
    dst.audio_bitrate_kbps = audio_br->value();
}

} // namespace

void refresh_destinations_table()
{
    if (!g_destinations_table) {
        return;
    }

    g_is_refreshing_table = true;
    std::unordered_map<std::string, RuntimeStatus> status_snapshot;
    {
        std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
        status_snapshot = g_runtime_statuses;
    }

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
        auto *protocol_item = new QTableWidgetItem(QString::fromStdString(dst.protocol));
        auto *vertical_item =
            new QTableWidgetItem(dst.requires_vertical ? tr_ms("VerticalYes") : tr_ms("VerticalNo"));
        auto *encoder_item = new QTableWidgetItem(encoder_cell_label(dst));
        auto *status_item = new QTableWidgetItem(QString::fromStdString(status_text_for_table(runtime, dst.enabled)));
        auto *error_item = new QTableWidgetItem(QString::fromStdString(error_text_for_table(runtime)));

        Qt::ItemFlags enabled_flags = (enabled_item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable;
        if (dst.is_default) {
            enabled_flags &= ~Qt::ItemIsUserCheckable;
            enabled_flags &= ~Qt::ItemIsEnabled;
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
        protocol_item->setFlags(protocol_item->flags() & ~Qt::ItemIsEditable);
        vertical_item->setFlags(vertical_item->flags() & ~Qt::ItemIsEditable);
        encoder_item->setFlags(encoder_item->flags() & ~Qt::ItemIsEditable);
        status_item->setFlags(status_item->flags() & ~Qt::ItemIsEditable);
        error_item->setFlags(error_item->flags() & ~Qt::ItemIsEditable);
        status_item->setForeground(status_color_for_table(runtime, dst.enabled));

        g_destinations_table->setItem(row, 0, enabled_item);
        g_destinations_table->setItem(row, 1, platform_item);
        g_destinations_table->setItem(row, 2, server_item);
        g_destinations_table->setItem(row, 3, protocol_item);
        g_destinations_table->setItem(row, 4, vertical_item);
        g_destinations_table->setItem(row, 5, encoder_item);
        g_destinations_table->setItem(row, 6, status_item);
        g_destinations_table->setItem(row, 7, error_item);
    }

    g_destinations_table->resizeColumnsToContents();
    g_is_refreshing_table = false;
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

    auto *form_widget = new QWidget(container);
    auto *form = new QFormLayout(form_widget);
    auto *platform = new QComboBox(form_widget);
    platform->addItem(tr_ms("PlatformYouTube"), QStringLiteral("YouTube"));
    platform->addItem(tr_ms("PlatformTwitch"), QStringLiteral("Twitch"));
    platform->addItem(tr_ms("PlatformKick"), QStringLiteral("Kick"));
    platform->addItem(tr_ms("PlatformOther"), QStringLiteral("Other"));
    auto *server = new QLineEdit(form_widget);
    server->setReadOnly(true);
    auto *stream_key = new QLineEdit(form_widget);
    stream_key->setEchoMode(QLineEdit::Password);
    auto *video_encoder = new QComboBox(form_widget);
    populate_video_encoder_combo(video_encoder);
    auto *video_bitrate = new QSpinBox(form_widget);
    video_bitrate->setRange(0, 500000);
    video_bitrate->setSingleStep(100);
    video_bitrate->setSpecialValueText(tr_ms("BitrateDefaultSpecial"));
    video_bitrate->setToolTip(tr_ms("VideoBitrateTip"));
    auto *audio_bitrate = new QSpinBox(form_widget);
    audio_bitrate->setRange(0, 512);
    audio_bitrate->setSpecialValueText(tr_ms("BitrateDefaultSpecial"));
    audio_bitrate->setToolTip(tr_ms("AudioBitrateTip"));

    form->addRow(tr_ms("LabelPlatform"), platform);
    form->addRow(tr_ms("LabelServer"), server);
    form->addRow(tr_ms("LabelStreamKey"), stream_key);
    form->addRow(tr_ms("LabelVideoEncoder"), video_encoder);
    form->addRow(tr_ms("LabelVideoBitrate"), video_bitrate);
    form->addRow(tr_ms("LabelAudioBitrate"), audio_bitrate);

    auto *add_button = new QPushButton(tr_ms("BtnAddDestination"), container);
    auto *edit_button = new QPushButton(tr_ms("BtnEditSelected"), container);
    auto *remove_button = new QPushButton(tr_ms("BtnRemoveSelected"), container);

    g_destinations_table = new QTableWidget(container);
    g_destinations_table->setColumnCount(8);
    g_destinations_table->setHorizontalHeaderLabels({tr_ms("ColEnabled"), tr_ms("ColPlatform"), tr_ms("ColServer"),
                                                   tr_ms("ColProtocol"), tr_ms("ColVertical"), tr_ms("ColEncoder"),
                                                   tr_ms("ColStatus"), tr_ms("ColLastError")});
    g_destinations_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    g_destinations_table->setSelectionMode(QAbstractItemView::SingleSelection);
    g_destinations_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    g_destinations_table->verticalHeader()->setVisible(false);
    g_destinations_table->horizontalHeader()->setStretchLastSection(true);

    auto sync_platform_dependent_ui = [platform, server]() {
        const QString id = platform->currentData().toString();
        const bool preset = (id == QStringLiteral("YouTube") || id == QStringLiteral("Twitch") ||
                               id == QStringLiteral("Kick"));
        server->setReadOnly(preset);
        if (preset) {
            const PlatformKind kind = detect_platform_kind(id.toStdString());
            server->setText(QString::fromStdString(default_server_for_platform(kind)));
        }
    };
    sync_platform_dependent_ui();

    QObject::connect(platform, &QComboBox::currentIndexChanged,
                     [sync_platform_dependent_ui](int) { sync_platform_dependent_ui(); });

    QObject::connect(add_button, &QPushButton::clicked,
                     [platform, server, stream_key, video_encoder, video_bitrate, audio_bitrate, sync_platform_dependent_ui]() {
                         Destination dst;
                         dst.platform = platform->currentData().toString().toStdString();
                         if (dst.platform.empty()) {
                             dst.platform = platform->currentText().toStdString();
                         }
                         dst.server = server->text().toStdString();
                         dst.stream_key = stream_key->text().toStdString();
                         fill_destination_from_form(dst, video_encoder, video_bitrate, audio_bitrate);

                         normalize_destination(dst);
                         if (!validate_and_log_destination(dst)) {
                             return;
                         }

                         g_destinations.push_back(std::move(dst));
                         {
                             std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
                             g_runtime_statuses[destination_id(g_destinations.back())] = RuntimeStatus{};
                         }
                         save_destinations();
                         refresh_destinations_table();

                         stream_key->clear();
                         video_bitrate->setValue(0);
                         audio_bitrate->setValue(0);
                         select_video_encoder_combo(video_encoder, "");
                         sync_platform_dependent_ui();
                         blog(LOG_INFO, "[obs-multistream-plugin] Destination added");
                     });

    QObject::connect(edit_button, &QPushButton::clicked,
                     [platform, server, stream_key, video_encoder, video_bitrate, audio_bitrate, sync_platform_dependent_ui]() {
                         if (!g_destinations_table) {
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

                         Destination updated = g_destinations[static_cast<size_t>(row)];
                         updated.platform = platform->currentData().toString().toStdString();
                         if (updated.platform.empty()) {
                             updated.platform = platform->currentText().toStdString();
                         }
                         updated.server = server->text().toStdString();
                         updated.stream_key = stream_key->text().toStdString();
                         fill_destination_from_form(updated, video_encoder, video_bitrate, audio_bitrate);

                         normalize_destination(updated);
                         if (!validate_and_log_destination(updated, row)) {
                             return;
                         }

                         Destination &existing = g_destinations[static_cast<size_t>(row)];
                         const std::string old_id = destination_id(existing);
                         const std::string new_id = destination_id(updated);

                         RuntimeStatus status;
                         {
                             std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
                             const auto status_it = g_runtime_statuses.find(old_id);
                             if (status_it != g_runtime_statuses.end()) {
                                 status = status_it->second;
                                 g_runtime_statuses.erase(status_it);
                             }
                         }

                         existing = std::move(updated);

                         {
                             std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
                             g_runtime_statuses[new_id] = status;
                         }
                         save_destinations();
                         refresh_destinations_table();
                         sync_platform_dependent_ui();
                         blog(LOG_INFO, "[obs-multistream-plugin] Destination edited for platform=%s",
                              existing.platform.c_str());
                     });

    QObject::connect(g_destinations_table, &QTableWidget::itemSelectionChanged,
                     [platform, server, stream_key, video_encoder, video_bitrate, audio_bitrate, sync_platform_dependent_ui]() {
                         if (!g_destinations_table) {
                             return;
                         }

                         const int row = g_destinations_table->currentRow();
                         if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
                             return;
                         }

                         const Destination &selected = g_destinations[static_cast<size_t>(row)];
                         apply_platform_row_to_combo(platform, selected.platform);
                         sync_platform_dependent_ui();
                         server->setText(QString::fromStdString(selected.server));
                         stream_key->setText(QString::fromStdString(selected.stream_key));
                         select_video_encoder_combo(video_encoder, selected.video_encoder_id);
                         video_bitrate->setValue(selected.video_bitrate_kbps);
                         audio_bitrate->setValue(selected.audio_bitrate_kbps);
                     });

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

    layout->addWidget(form_widget);
    layout->addWidget(add_button);
    layout->addWidget(g_destinations_table);
    layout->addLayout(manage_layout);
    layout->addStretch();

    sync_default_destination_from_obs();
    refresh_destinations_table();

    for (int delay_ms : {0, 250, 750, 2000, 5000}) {
        QTimer::singleShot(delay_ms, container, []() {
            sync_default_destination_from_obs();
        });
    }

    g_dock = new QDockWidget(tr_ms("DockTitle"), main_window);
    g_dock->setObjectName("obs_multistream_destinations_dock");
    g_dock->setWidget(container);

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
}
