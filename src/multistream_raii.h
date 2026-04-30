#pragma once

#include <obs.h>

namespace obs_multistream_detail {

struct ObsServiceHolder {
    obs_service_t *p = nullptr;
    ~ObsServiceHolder()
    {
        clear();
    }
    ObsServiceHolder(const ObsServiceHolder &) = delete;
    ObsServiceHolder &operator=(const ObsServiceHolder &) = delete;
    void clear()
    {
        if (p) {
            obs_service_release(p);
            p = nullptr;
        }
    }
    obs_service_t *release()
    {
        obs_service_t *t = p;
        p = nullptr;
        return t;
    }
};

struct ObsOutputHolder {
    obs_output_t *p = nullptr;
    ~ObsOutputHolder()
    {
        clear();
    }
    ObsOutputHolder(const ObsOutputHolder &) = delete;
    ObsOutputHolder &operator=(const ObsOutputHolder &) = delete;
    void clear()
    {
        if (p) {
            obs_output_release(p);
            p = nullptr;
        }
    }
    obs_output_t *release()
    {
        obs_output_t *t = p;
        p = nullptr;
        return t;
    }
};

struct ObsEncoderHolder {
    obs_encoder_t *p = nullptr;
    ~ObsEncoderHolder()
    {
        clear();
    }
    ObsEncoderHolder(const ObsEncoderHolder &) = delete;
    ObsEncoderHolder &operator=(const ObsEncoderHolder &) = delete;
    void clear()
    {
        if (p) {
            obs_encoder_release(p);
            p = nullptr;
        }
    }
    obs_encoder_t *release()
    {
        obs_encoder_t *t = p;
        p = nullptr;
        return t;
    }
};

} // namespace obs_multistream_detail
