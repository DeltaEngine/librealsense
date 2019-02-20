// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2018 Intel Corporation. All Rights Reserved.

#pragma once

#include <vector>
#include <mutex>
#include <string>

#include "device.h"
#include "context.h"
#include "backend.h"
#include "hw-monitor.h"
#include "image.h"
#include "stream.h"
#include "environment.h"
#include "core/debug.h"
#include "l500-private.h"
#include "proc/decimation-filter.h"
#include "proc/threshold.h" 
#include "proc/spatial-filter.h"
#include "proc/temporal-filter.h"
#include "proc/hole-filling-filter.h"
#include "proc/zero-order.h"

namespace librealsense
{
    const uint16_t L500_PID = 0x0b0d;

    class l500_device;

    class l500_timestamp_reader : public frame_timestamp_reader
    {
        static const int pins = 3;
        mutable std::vector<int64_t> counter;
        std::shared_ptr<platform::time_service> _ts;
        mutable std::recursive_mutex _mtx;
    public:
        l500_timestamp_reader(std::shared_ptr<platform::time_service> ts)
            : counter(pins), _ts(ts)
        {
            reset();
        }

        void reset() override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            for (auto i = 0; i < pins; ++i)
            {
                counter[i] = 0;
            }
        }

        rs2_time_t get_frame_timestamp(const request_mapping& mode, const platform::frame_object& fo) override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            return _ts->get_time();
        }

        unsigned long long get_frame_counter(const request_mapping & mode, const platform::frame_object& fo) const override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto pin_index = 0;
            if (mode.pf->fourcc == 0x5a313620) // Z16
                pin_index = 1;
            else if (mode.pf->fourcc == 0x43202020) // Confidence
                pin_index = 2;

            return ++counter[pin_index];
        }

        rs2_timestamp_domain get_frame_timestamp_domain(const request_mapping & mode, const platform::frame_object& fo) const override
        {
            return RS2_TIMESTAMP_DOMAIN_SYSTEM_TIME;
        }
    };

    class l500_timestamp_reader_from_metadata : public frame_timestamp_reader
    {
        std::unique_ptr<l500_timestamp_reader> _backup_timestamp_reader;
        bool one_time_note;
        mutable std::recursive_mutex _mtx;
        arithmetic_wraparound<uint32_t, uint64_t > ts_wrap;

    protected:

        bool has_metadata_ts(const platform::frame_object& fo) const
        {
            // Metadata support for a specific stream is immutable
            const bool has_md_ts = [&] { std::lock_guard<std::recursive_mutex> lock(_mtx);
            return ((fo.metadata != nullptr) && (fo.metadata_size >= platform::uvc_header_size) && ((byte*)fo.metadata)[0] >= platform::uvc_header_size);
            }();

            return has_md_ts;
        }

        bool has_metadata_fc(const platform::frame_object& fo) const
        {
            // Metadata support for a specific stream is immutable
            const bool has_md_frame_counter = [&] { std::lock_guard<std::recursive_mutex> lock(_mtx);
            return ((fo.metadata != nullptr) && (fo.metadata_size > platform::uvc_header_size) && ((byte*)fo.metadata)[0] > platform::uvc_header_size);
            }();

            return has_md_frame_counter;
        }

    public:
        l500_timestamp_reader_from_metadata(std::shared_ptr<platform::time_service> ts) :_backup_timestamp_reader(nullptr), one_time_note(false)
        {
            _backup_timestamp_reader = std::unique_ptr<l500_timestamp_reader>(new l500_timestamp_reader(ts));
            reset();
        }

        rs2_time_t get_frame_timestamp(const request_mapping& mode, const platform::frame_object& fo) override;

        unsigned long long get_frame_counter(const request_mapping & mode, const platform::frame_object& fo) const override;

        void reset() override;

        rs2_timestamp_domain get_frame_timestamp_domain(const request_mapping & mode, const platform::frame_object& fo) const override;
    };

    class l500_info : public device_info
    {
    public:
        std::shared_ptr<device_interface> create(std::shared_ptr<context> ctx,
            bool register_device_notifications) const override;

        l500_info(std::shared_ptr<context> ctx,
                  std::vector<platform::uvc_device_info> depth,
                  platform::usb_device_info hwm)
            : device_info(ctx),
              _depth(std::move(depth)),
              _hwm(std::move(hwm))
        {}

        static std::vector<std::shared_ptr<device_info>> pick_l500_devices(
            std::shared_ptr<context> ctx,
            std::vector<platform::uvc_device_info>& platform,
            std::vector<platform::usb_device_info>& usb);

        platform::backend_device_group get_device_data() const override
        {
            return platform::backend_device_group({ _depth }, { _hwm });
        }

    private:
        std::vector<platform::uvc_device_info> _depth;
        platform::usb_device_info _hwm;
    };

    class l500_device;
    class zo_point_option_x : public option_base
    {
    public:
        zo_point_option_x(int min, int max, int step, int def, l500_device* owner, const std::string& desc)
            : option_base({ static_cast<float>(min),
                           static_cast<float>(max),
                           static_cast<float>(step),
                           static_cast<float>(def) }), _owner(owner), _value(0)
        {}

        float query() const override;

        void set(float value) override
        {
            _value = static_cast<int>(value);
        }

        bool is_enabled() const override { return true; }
        const char* get_description() const override { return _desc.c_str(); }

    private:
        l500_device* _owner;
        mutable int _value = 0;
        std::string _desc;
    };

    class zo_point_option_y : public option_base
    {
    public:
        zo_point_option_y(int min, int max, int step, int def, l500_device* owner, const std::string& desc)
            : option_base({ static_cast<float>(min),
                           static_cast<float>(max),
                           static_cast<float>(step),
                           static_cast<float>(def)}), _owner(owner), _value(0)
        {}

        float query() const override;


        void set(float value) override
        {
            _value = static_cast<int>(value);
        }

        bool is_enabled() const override { return true; }
        const char* get_description() const override { return _desc.c_str(); }

    private:
        l500_device* _owner;
        mutable int _value = 0;
        std::string _desc;
    };

    class l500_device final : public virtual device, public debug_interface
    {
    public:

        class l500_depth_sensor : public uvc_sensor, public video_sensor_interface, public depth_sensor
        {
        public:
            explicit l500_depth_sensor(l500_device* owner, std::shared_ptr<platform::uvc_device> uvc_device,
                                       std::unique_ptr<frame_timestamp_reader> timestamp_reader)
                : uvc_sensor("L500 Depth Sensor", uvc_device, move(timestamp_reader), owner), _owner(owner)
            {

                _zo_point_x_option = std::make_shared<zo_point_option_x>(
                    20,
                    640,
                    1,
                    0,
                    owner,
                    "zero order point x");

                register_option(static_cast<rs2_option>(RS2_OPTION_ZO_POINT_X), _zo_point_x_option);

                _zo_point_y_option = std::make_shared<zo_point_option_y>(
                    20,
                    640,
                    1,
                    0,
                    owner,
                    "zero order point x");

                register_option(static_cast<rs2_option>(RS2_OPTION_ZO_POINT_Y), _zo_point_y_option);

            }

            rs2_intrinsics get_intrinsics(const stream_profile& profile) const override
            {
                auto res = *_owner->_calib_table_raw;
                auto intr = (float*)res.data();

                if (res.size() < sizeof(float) * 4)
                    throw invalid_value_exception("size of calibration invalid");

                rs2_intrinsics intrinsics;
                intrinsics.width = profile.width;
                intrinsics.height = profile.height;
                intrinsics.fx = std::abs(intr[0]);
                intrinsics.fy = std::abs(intr[1]);
                intrinsics.ppx = std::abs(intr[2]);
                intrinsics.ppy = std::abs(intr[3]);
                return intrinsics;
            }

            stream_profiles init_stream_profiles() override
            {
                auto lock = environment::get_instance().get_extrinsics_graph().lock();

                auto results = uvc_sensor::init_stream_profiles();
                for (auto p : results)
                {
                    // Register stream types
                    if (p->get_stream_type() == RS2_STREAM_DEPTH)
                    {
                        assign_stream(_owner->_depth_stream, p);
                    }
                    else if (p->get_stream_type() == RS2_STREAM_INFRARED)
                    {
                        assign_stream(_owner->_ir_stream, p);
                    }
                    else if (p->get_stream_type() == RS2_STREAM_CONFIDENCE)
                    {
                        assign_stream(_owner->_confidence_stream, p);
                    }

                    // Register intrinsics
                    auto video = dynamic_cast<video_stream_profile_interface*>(p.get());

                    auto profile = to_profile(p.get());
                    std::weak_ptr<l500_depth_sensor> wp =
                        std::dynamic_pointer_cast<l500_depth_sensor>(this->shared_from_this());

                    video->set_intrinsics([profile, wp]()
                    {
                        auto sp = wp.lock();
                        if (sp)
                            return sp->get_intrinsics(profile);
                        else
                            return rs2_intrinsics{};
                    });
                }

                return results;
            }

            float get_depth_scale() const override {  return get_option(RS2_OPTION_DEPTH_UNITS).query(); }

            void create_snapshot(std::shared_ptr<depth_sensor>& snapshot) const  override
            {
                snapshot = std::make_shared<depth_sensor_snapshot>(get_depth_scale());
            }
            void enable_recording(std::function<void(const depth_sensor&)> recording_function) override
            {
                get_option(RS2_OPTION_DEPTH_UNITS).enable_recording([this, recording_function](const option& o) {
                    recording_function(*this);
                });
            }

            static processing_blocks get_l500_recommended_proccesing_blocks(std::shared_ptr<option> zo_point_x, std::shared_ptr<option> zo_point_y)
            {
                processing_blocks res;

                res.push_back(std::make_shared<zero_order>(zo_point_x, zo_point_y));
                auto dec = std::make_shared<decimation_filter>();
                if (dec->supports_option(RS2_OPTION_STREAM_FILTER))
                {
                    dec->get_option(RS2_OPTION_STREAM_FILTER).set(RS2_STREAM_DEPTH);
                    dec->get_option(RS2_OPTION_STREAM_FORMAT_FILTER).set(RS2_FORMAT_Z16);
                    res.push_back(dec);
                }
                res.push_back(std::make_shared<threshold>());
                res.push_back(std::make_shared<spatial_filter>());
                res.push_back(std::make_shared<temporal_filter>());
                res.push_back(std::make_shared<hole_filling_filter>());
                return res;
            }

            processing_blocks get_recommended_processing_blocks() const override
            {
                return get_l500_recommended_proccesing_blocks(_zo_point_x_option, _zo_point_y_option);
            };

            std::shared_ptr<option> get_zo_point_x() const
            {
                return _zo_point_x_option;
            }
            std::shared_ptr<option> get_zo_point_y() const
            {
                return _zo_point_y_option;
            }
        private:
            const l500_device* _owner;
            float _depth_units;
            std::shared_ptr<option> _zo_point_x_option;
            std::shared_ptr<option> _zo_point_y_option;
        };

        std::shared_ptr<uvc_sensor> create_depth_device(std::shared_ptr<context> ctx,
                                                        const std::vector<platform::uvc_device_info>& all_device_infos)
        {
            using namespace ivcam2;
            auto&& backend = ctx->get_backend();

            std::vector<std::shared_ptr<platform::uvc_device>> depth_devices;
            for (auto&& info : filter_by_mi(all_device_infos, 0)) // Filter just mi=0, DEPTH
                depth_devices.push_back(backend.create_uvc_device(info));

            auto depth_ep = std::make_shared<l500_depth_sensor>(this, std::make_shared<platform::multi_pins_uvc_device>(depth_devices),
                std::unique_ptr<frame_timestamp_reader>(new l500_timestamp_reader_from_metadata(backend.create_time_service())));
                                                               // std::unique_ptr<frame_timestamp_reader>(new l500_timestamp_reader(backend.create_time_service())));

            depth_ep->register_xu(depth_xu);
            depth_ep->register_pixel_format(pf_z16_l500);
            depth_ep->register_pixel_format(pf_confidence_l500);
            depth_ep->register_pixel_format(pf_y8_l500);

            depth_ep->register_option(RS2_OPTION_LASER_POWER,
                std::make_shared<uvc_xu_option<int>>(
                    *depth_ep,
                    ivcam2::depth_xu,
                    ivcam2::IVCAM2_DEPTH_LASER_POWER, "Power of the l500 projector, with 0 meaning projector off"));



            return depth_ep;
        }

        std::vector<uint8_t> send_receive_raw_data(const std::vector<uint8_t>& input) override
        {
            return _hw_monitor->send(input);
        }

        void hardware_reset() override
        {
            force_hardware_reset();
        }

        uvc_sensor& get_depth_sensor() { return dynamic_cast<uvc_sensor&>(get_sensor(_depth_device_idx)); }

        std::vector<uint8_t> get_raw_calibration_table() const;

        l500_device(std::shared_ptr<context> ctx,
                    const platform::backend_device_group& group,
                    bool register_device_notifications);

        void create_snapshot(std::shared_ptr<debug_interface>& snapshot) const override;
        void enable_recording(std::function<void(const debug_interface&)> record_action) override;
        std::vector<tagged_profile> get_profiles_tags() const override;

        virtual std::shared_ptr<matcher> create_matcher(const frame_holder& frame) const override;

        bool try_read_zo_point_x(int* zo_x);
        bool try_read_zo_point_y(int* zo_y);
        void read_zo_point(int* zo_x, int* zo_y);
        int  read_algo_version();
        float  read_baseline();
        float  read_znorm();

    private:
        const uint8_t _depth_device_idx;
        std::shared_ptr<hw_monitor> _hw_monitor;
        std::shared_ptr<stream_interface> _depth_stream;
        std::shared_ptr<stream_interface> _ir_stream;
        std::shared_ptr<stream_interface> _confidence_stream;

        lazy<std::vector<uint8_t>> _calib_table_raw;

        void force_hardware_reset() const;
    };
}
