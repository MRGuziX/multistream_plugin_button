#include <obs-module.h>
#include <obs-frontend-api.h>

#include <functional>
#include <memory>
#include <QDockWidget>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <vector>

namespace {
struct Destination {
    std::string platform;
    std::string server;
    std::string stream_key;
    bool enabled = true;
};

std::vector<Destination> g_destinations;
QDockWidget *g_dock = nullptr;

class MultistreamManager {
public:
    void start_for_all_enabled(const std::vector<Destination> &destinations)
    {
        stop_all();

        obs_output_t *main_output = obs_frontend_get_streaming_output();
        if (!main_output) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Main streaming output is not available");
            return;
        }

        obs_encoder_t *video_encoder = obs_output_get_video_encoder(main_output);
        obs_encoder_t *audio_encoder = obs_output_get_audio_encoder(main_output, 0);
        if (!video_encoder || !audio_encoder) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Main stream encoders are not available");
            obs_output_release(main_output);
            return;
        }

        bool started_any = false;
        for (const Destination &dst : destinations) {
            if (!dst.enabled) {
                continue;
            }

            const bool started = start_single_destination(dst, video_encoder, audio_encoder);
            started_any = started_any || started;
        }

        if (!started_any) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No secondary destination started");
        }

        obs_output_release(main_output);
    }

    void stop_all()
    {
        for (DestinationRuntime &runtime : runtimes_) {
            if (runtime.output) {
                if (obs_output_active(runtime.output)) {
                    obs_output_stop(runtime.output);
                }
                obs_output_release(runtime.output);
                runtime.output = nullptr;
            }

            if (runtime.service) {
                obs_service_release(runtime.service);
                runtime.service = nullptr;
            }
        }

        runtimes_.clear();
    }

private:
    struct DestinationRuntime {
        std::string platform;
        obs_service_t *service = nullptr;
        obs_output_t *output = nullptr;
    };

    std::vector<DestinationRuntime> runtimes_;

    static std::string make_safe_name(const std::string &prefix, const Destination &dst)
    {
        const std::string platform = dst.platform.empty() ? "unknown" : dst.platform;
        return prefix + "_" + platform;
    }

    bool start_single_destination(const Destination &dst, obs_encoder_t *video_encoder, obs_encoder_t *audio_encoder)
    {
        obs_data_t *service_settings = obs_data_create();
        obs_data_set_string(service_settings, "server", dst.server.c_str());
        obs_data_set_string(service_settings, "key", dst.stream_key.c_str());

        const std::string service_name = make_safe_name("multistream_service", dst);
        obs_service_t *service = obs_service_create("rtmp_custom", service_name.c_str(), service_settings, nullptr);
        obs_data_release(service_settings);

        if (!service) {
            blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create service for platform=%s", dst.platform.c_str());
            return false;
        }

        const std::string output_name = make_safe_name("multistream_output", dst);
        obs_output_t *output = obs_output_create("rtmp_output", output_name.c_str(), nullptr, nullptr);
        if (!output) {
            blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create output for platform=%s", dst.platform.c_str());
            obs_service_release(service);
            return false;
        }

        obs_output_set_service(output, service);
        obs_output_set_video_encoder(output, video_encoder);
        obs_output_set_audio_encoder(output, audio_encoder, 0);

        if (!obs_output_start(output)) {
            blog(LOG_ERROR, "[obs-multistream-plugin] Failed to start output for platform=%s", dst.platform.c_str());
            obs_output_release(output);
            obs_service_release(service);
            return false;
        }

        DestinationRuntime runtime;
        runtime.platform = dst.platform;
        runtime.service = service;
        runtime.output = output;
        runtimes_.push_back(std::move(runtime));

        blog(LOG_INFO, "[obs-multistream-plugin] Started secondary stream for platform=%s", dst.platform.c_str());
        return true;
    }
};

std::unique_ptr<MultistreamManager> g_multistream_manager;

void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (!g_multistream_manager) {
        return;
    }

    switch (event) {
    case OBS_FRONTEND_EVENT_STREAMING_STARTED:
        g_multistream_manager->start_for_all_enabled(g_destinations);
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
    case OBS_FRONTEND_EVENT_EXIT:
        g_multistream_manager->stop_all();
        break;
    default:
        break;
    }
}

void load_destinations()
{
    g_destinations.clear();

    obs_data_t *config = obs_data_create_from_json_file_safe("obs-multistream-plugin.json", "bak");
    if (!config) {
        blog(LOG_INFO, "[obs-multistream-plugin] No existing destination config found");
        return;
    }

    obs_data_array_t *items = obs_data_get_array(config, "destinations");
    const size_t count = obs_data_array_count(items);
    for (size_t i = 0; i < count; ++i) {
        obs_data_t *item = obs_data_array_item(items, i);
        Destination dst;
        dst.platform = obs_data_get_string(item, "platform");
        dst.server = obs_data_get_string(item, "server");
        dst.stream_key = obs_data_get_string(item, "stream_key");
        dst.enabled = obs_data_get_bool(item, "enabled");
        g_destinations.push_back(std::move(dst));
        obs_data_release(item);
    }

    obs_data_array_release(items);
    obs_data_release(config);
    blog(LOG_INFO, "[obs-multistream-plugin] Loaded %zu destination(s)", g_destinations.size());
}

void save_destinations()
{
    obs_data_t *config = obs_data_create();
    obs_data_array_t *items = obs_data_array_create();

    for (const Destination &dst : g_destinations) {
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "platform", dst.platform.c_str());
        obs_data_set_string(item, "server", dst.server.c_str());
        obs_data_set_string(item, "stream_key", dst.stream_key.c_str());
        obs_data_set_bool(item, "enabled", dst.enabled);
        obs_data_array_push_back(items, item);
        obs_data_release(item);
    }

    obs_data_set_array(config, "destinations", items);
    obs_data_save_json_safe(config, "obs-multistream-plugin.json", "tmp", "bak");

    obs_data_array_release(items);
    obs_data_release(config);
    blog(LOG_INFO, "[obs-multistream-plugin] Saved %zu destination(s)", g_destinations.size());
}

void create_dock()
{
    auto *main_window = reinterpret_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main_window) {
        blog(LOG_WARNING, "[obs-multistream-plugin] Could not get OBS main window");
        return;
    }

    auto *container = new QWidget(main_window);
    auto *layout = new QVBoxLayout(container);
    auto *hint = new QLabel("Milestone 2 scaffold: destination UI and persistence are initialized.", container);
    hint->setWordWrap(true);

    auto *form_widget = new QWidget(container);
    auto *form = new QFormLayout(form_widget);
    auto *platform = new QLineEdit(form_widget);
    auto *server = new QLineEdit(form_widget);
    auto *stream_key = new QLineEdit(form_widget);
    stream_key->setEchoMode(QLineEdit::Password);

    form->addRow("Platform", platform);
    form->addRow("Server", server);
    form->addRow("Stream key", stream_key);

    auto *add_button = new QPushButton("Add destination", container);

    QObject::connect(add_button, &QPushButton::clicked, [platform, server, stream_key]() {
        Destination dst;
        dst.platform = platform->text().toStdString();
        dst.server = server->text().toStdString();
        dst.stream_key = stream_key->text().toStdString();

        if (dst.platform.empty() || dst.server.empty() || dst.stream_key.empty()) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Platform/server/key cannot be empty");
            return;
        }

        g_destinations.push_back(std::move(dst));
        save_destinations();

        platform->clear();
        server->clear();
        stream_key->clear();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination added");
    });

    layout->addWidget(hint);
    layout->addWidget(form_widget);
    layout->addWidget(add_button);
    layout->addStretch();

    g_dock = new QDockWidget("Multistream Destinations", main_window);
    g_dock->setObjectName("obs_multistream_destinations_dock");
    g_dock->setWidget(container);

    main_window->addDockWidget(Qt::RightDockWidgetArea, g_dock);
}

void destroy_dock()
{
    if (!g_dock) {
        return;
    }

    g_dock->hide();
    g_dock->deleteLater();
    g_dock = nullptr;
}
} // namespace

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multistream-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS plugin for multistreaming to multiple RTMP platforms with shared encoders.";
}

bool obs_module_load(void)
{
    load_destinations();
    create_dock();
    g_multistream_manager = std::make_unique<MultistreamManager>();
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    blog(LOG_INFO, "[obs-multistream-plugin] Module loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    if (g_multistream_manager) {
        g_multistream_manager->stop_all();
    }
    g_multistream_manager.reset();
    save_destinations();
    destroy_dock();
    blog(LOG_INFO, "[obs-multistream-plugin] Module unloaded");
}
